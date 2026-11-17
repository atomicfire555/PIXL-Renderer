// [PerfProbe] sampling profiler for the render thread.
//
// A background thread wakes about every millisecond, briefly suspends the
// thread that presents frames, records its instruction pointer and a copy of
// the top of its stack, and resumes it. While the target is suspended the
// sampler only copies memory into preallocated buffers (no locks, no heap),
// so it cannot deadlock against the game. Analysis happens after resume:
//   - "leaf": which DLL the thread was executing in (exclusive time)
//   - "on stack": which DLLs had a return address on the stack (inclusive)
//   - the nearest PIXLRenderer.dll and SkyrimSE.exe return addresses, logged
//     as RVAs so they can be symbolized offline with the matching PDB.
// Nothing here touches rendering; the only cost is the sampling itself.

#include "PerfProbe.h"

#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cstring>
#include <functional>
#include <iterator>
#include <format>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace PerfProbe
{
	namespace
	{
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
		constexpr DWORD kHighResolutionTimerFlag = 0x00000002;
#else
		constexpr DWORD kHighResolutionTimerFlag = CREATE_WAITABLE_TIMER_HIGH_RESOLUTION;
#endif
		constexpr size_t kStackCopyBytes = 32 * 1024;
		constexpr double kSamplerReportSeconds = 10.0;
		constexpr size_t kMaxModules = 512;

		struct CodeRange
		{
			uintptr_t begin = 0;
			uintptr_t end = 0;
			uint16_t moduleIndex = 0;
		};

		struct SampleTarget
		{
			HANDLE thread = nullptr;
			uintptr_t stackLow = 0;
			uintptr_t stackHigh = 0;
			DWORD threadId = 0;
		};
		std::atomic<const SampleTarget*> currentTarget{ nullptr };

		struct ModuleEntry
		{
			std::string name;
			uintptr_t base = 0;
			// Sorted (rva, name) of exported functions, used to name leaf
			// samples inside system / third-party DLLs without symbols.
			std::vector<std::pair<uint32_t, const char*>> exports;
		};

		void LoadExports(ModuleEntry& a_module, const IMAGE_NT_HEADERS* a_nt)
		{
			const auto& directory = a_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
			if (!directory.VirtualAddress || !directory.Size)
				return;
			const auto base = a_module.base;
			const auto* exportDir = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
			const auto* functions = reinterpret_cast<const DWORD*>(base + exportDir->AddressOfFunctions);
			const auto* names = reinterpret_cast<const DWORD*>(base + exportDir->AddressOfNames);
			const auto* ordinals = reinterpret_cast<const WORD*>(base + exportDir->AddressOfNameOrdinals);
			for (DWORD i = 0; i < exportDir->NumberOfNames; ++i) {
				if (ordinals[i] >= exportDir->NumberOfFunctions)
					continue;
				const DWORD rva = functions[ordinals[i]];
				if (rva >= directory.VirtualAddress && rva < directory.VirtualAddress + directory.Size)
					continue;  // forwarder string, not code
				a_module.exports.emplace_back(rva, reinterpret_cast<const char*>(base + names[i]));
			}
			std::ranges::sort(a_module.exports, [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
		}

		class RenderThreadSampler
		{
		public:
			RenderThreadSampler() :
				stackCopy(kStackCopyBytes / sizeof(uintptr_t))
			{}

			void Run()
			{
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
				HANDLE samplerTimer = CreateWaitableTimerExW(nullptr, nullptr, kHighResolutionTimerFlag, TIMER_ALL_ACCESS);
				if (!samplerTimer)
					samplerTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
				if (!samplerTimer) {
					logger::warn("[PerfProbe] Sampler could not create a timer; sampling disabled");
					return;
				}
				LARGE_INTEGER firstDue{};
				firstDue.QuadPart = -10000;  // 1 ms, relative
				SetWaitableTimer(samplerTimer, &firstDue, 1, nullptr, nullptr, FALSE);

				RebuildModules();
				LARGE_INTEGER frequency{};
				QueryPerformanceFrequency(&frequency);
				int64_t windowStart = Now();
				logger::info("[PerfProbe] Render-thread sampler running (~1 kHz, report every {:.0f}s)", kSamplerReportSeconds);

				for (;;) {
					WaitForSingleObject(samplerTimer, INFINITE);
					SampleOnce();
					const int64_t sampleNow = Now();
					if (static_cast<double>(sampleNow - windowStart) / static_cast<double>(frequency.QuadPart) >= kSamplerReportSeconds) {
						Report(static_cast<double>(sampleNow - windowStart) / static_cast<double>(frequency.QuadPart));
						windowStart = sampleNow;
						RebuildModules();
					}
				}
			}

		private:
			void RebuildModules()
			{
				modules.clear();
				ranges.clear();
				std::vector<HMODULE> handles(1024);
				DWORD needed = 0;
				if (!K32EnumProcessModules(GetCurrentProcess(), handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed))
					return;
				handles.resize(std::min<size_t>(needed / sizeof(HMODULE), handles.size()));
				const auto gameModule = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
				HMODULE self = nullptr;
				GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(&PerfProbe::StartSampler), &self);
				gameIndex = -1;
				pixlIndex = -1;
				for (HMODULE handle : handles) {
					if (modules.size() >= kMaxModules)
						break;
					char nameBuffer[MAX_PATH]{};
					K32GetModuleBaseNameA(GetCurrentProcess(), handle, nameBuffer, MAX_PATH);
					const auto base = reinterpret_cast<uintptr_t>(handle);
					const auto moduleIndex = static_cast<uint16_t>(modules.size());
					modules.push_back({ nameBuffer, base });
					if (base == gameModule)
						gameIndex = moduleIndex;
					if (handle == self)
						pixlIndex = moduleIndex;
					const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
					if (dos->e_magic != IMAGE_DOS_SIGNATURE)
						continue;
					const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + static_cast<uintptr_t>(dos->e_lfanew));
					if (nt->Signature != IMAGE_NT_SIGNATURE)
						continue;
					if (moduleIndex != gameIndex && moduleIndex != pixlIndex)
						LoadExports(modules.back(), nt);
					const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
					for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
						if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
							continue;
						const uintptr_t sectionSize = std::max<uintptr_t>(section->Misc.VirtualSize, section->SizeOfRawData);
						ranges.push_back({ base + section->VirtualAddress, base + section->VirtualAddress + sectionSize, moduleIndex });
					}
				}
				std::ranges::sort(ranges, [](const CodeRange& lhs, const CodeRange& rhs) { return lhs.begin < rhs.begin; });
			}

			int Lookup(uintptr_t address) const
			{
				auto found = std::upper_bound(ranges.begin(), ranges.end(), address,
					[](uintptr_t value, const CodeRange& range) { return value < range.begin; });
				if (found == ranges.begin())
					return -1;
				--found;
				return address < found->end ? found->moduleIndex : -1;
			}

			void SampleOnce()
			{
				const SampleTarget* sampled = currentTarget.load(std::memory_order_acquire);
				if (!sampled)
					return;
				const HANDLE target = sampled->thread;
				const uintptr_t stackLow = sampled->stackLow;
				const uintptr_t stackHigh = sampled->stackHigh;
				if (SuspendThread(target) == static_cast<DWORD>(-1))
					return;
				CONTEXT context{};
				context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
				const bool haveContext = GetThreadContext(target, &context) != FALSE;
				size_t copiedWords = 0;
				if (haveContext && context.Rsp >= stackLow && context.Rsp < stackHigh) {
					const size_t available = static_cast<size_t>(stackHigh - context.Rsp);
					const size_t bytes = std::min(available, kStackCopyBytes) & ~static_cast<size_t>(7);
					std::memcpy(stackCopy.data(), reinterpret_cast<const void*>(context.Rsp), bytes);
					copiedWords = bytes / sizeof(uintptr_t);
				}
				ResumeThread(target);
				if (!haveContext)
					return;

				++totalSamples;
				const int leaf = Lookup(context.Rip);
				exclusive[leaf < 0 ? kMaxModules : static_cast<size_t>(leaf)]++;

				std::bitset<kMaxModules> seen;
				if (leaf >= 0)
					seen.set(static_cast<size_t>(leaf));
				uintptr_t nearestPixl = (leaf >= 0 && leaf == pixlIndex) ? context.Rip : 0;
				uintptr_t nearestGame = (leaf >= 0 && leaf == gameIndex) ? context.Rip : 0;
				for (size_t i = 0; i < copiedWords; ++i) {
					const uintptr_t value = stackCopy[i];
					const int owner = Lookup(value);
					if (owner < 0)
						continue;
					seen.set(static_cast<size_t>(owner));
					if (!nearestPixl && owner == pixlIndex)
						nearestPixl = value;
					if (!nearestGame && owner == gameIndex)
						nearestGame = value;
				}
				for (size_t i = 0; i < modules.size(); ++i)
					if (seen.test(i))
						inclusive[i]++;
				if (nearestPixl && pixlIndex >= 0) {
					const auto pixlRva = static_cast<uint32_t>(nearestPixl - modules[static_cast<size_t>(pixlIndex)].base);
					if (leaf == pixlIndex)
						pixlSelf[pixlRva]++;
					else
						pixlSites[pixlRva]++;
				}
				if (leaf >= 0 && leaf != gameIndex && leaf != pixlIndex) {
					const auto& owner = modules[static_cast<size_t>(leaf)];
					const auto rva = static_cast<uint32_t>(context.Rip - owner.base);
					const char* exportName = "?";
					auto found = std::upper_bound(owner.exports.begin(), owner.exports.end(), rva,
						[](uint32_t value, const auto& entry) { return value < entry.first; });
					if (found != owner.exports.begin())
						exportName = std::prev(found)->second;
					auto& bucket = leafExports[{ static_cast<uint32_t>(leaf), exportName }];
					bucket.samples++;
					if (nearestPixl && pixlIndex >= 0)
						bucket.pixlCallers[static_cast<uint32_t>(nearestPixl - modules[static_cast<size_t>(pixlIndex)].base)]++;
					if (nearestGame && gameIndex >= 0)
						bucket.gameCallers[static_cast<uint32_t>(nearestGame - modules[static_cast<size_t>(gameIndex)].base)]++;
				}
				if (leaf >= 0 && leaf != gameIndex && leaf != pixlIndex && nearestGame && gameIndex >= 0) {
					const uint64_t key = (static_cast<uint64_t>(leaf) << 32) |
					                     static_cast<uint32_t>(nearestGame - modules[static_cast<size_t>(gameIndex)].base);
					externalCalls[key]++;
				}
				if (leaf >= 0 && leaf == gameIndex)
					gameLeaf[static_cast<uint32_t>(context.Rip - modules[static_cast<size_t>(gameIndex)].base)]++;
			}

			std::string ModuleName(size_t index) const
			{
				return index < modules.size() ? modules[index].name : std::string("<unknown>");
			}

			template <class Map, class Format>
			static std::string TopEntries(const Map& map, size_t limit, double total, Format&& format)
			{
				std::vector<std::pair<typename Map::key_type, uint32_t>> sortedEntries(map.begin(), map.end());
				std::ranges::sort(sortedEntries, [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });
				std::string text;
				for (size_t i = 0; i < sortedEntries.size() && i < limit; ++i)
					text += format(sortedEntries[i].first, 100.0 * sortedEntries[i].second / total);
				return text;
			}

			void Report(double seconds)
			{
				if (totalSamples == 0)
					return;
				const double total = static_cast<double>(totalSamples);
				std::vector<std::pair<uint32_t, size_t>> leafOrder;
				std::vector<std::pair<uint32_t, size_t>> stackOrder;
				for (size_t i = 0; i <= kMaxModules; ++i)
					if (exclusive[i])
						leafOrder.emplace_back(exclusive[i], i);
				for (size_t i = 0; i < kMaxModules; ++i)
					if (inclusive[i])
						stackOrder.emplace_back(inclusive[i], i);
				std::ranges::sort(leafOrder, std::greater<>());
				std::ranges::sort(stackOrder, std::greater<>());
				std::string leafText;
				for (size_t i = 0; i < leafOrder.size() && i < 14; ++i)
					leafText += std::format(" {}={:.1f}%", leafOrder[i].second == kMaxModules ? std::string("<no module>") : ModuleName(leafOrder[i].second),
						100.0 * leafOrder[i].first / total);
				std::string stackText;
				for (size_t i = 0; i < stackOrder.size() && i < 14; ++i)
					stackText += std::format(" {}={:.1f}%", ModuleName(stackOrder[i].second), 100.0 * stackOrder[i].first / total);

				logger::info("[PerfProbe] Sampler {:.1f}s, {} samples of the render thread", seconds, totalSamples);
				logger::info("[PerfProbe]   executing in (exclusive):{}", leafText);
				logger::info("[PerfProbe]   on call stack (inclusive):{}", stackText);
				logger::info("[PerfProbe]   PIXLRenderer.dll executing (RVA):{}",
					TopEntries(pixlSelf, 50, total, [](uint32_t rva, double percent) { return std::format(" {:X}={:.2f}%", rva, percent); }));
				logger::info("[PerfProbe]   other code under nearest PIXLRenderer.dll site (RVA):{}",
					TopEntries(pixlSites, 40, total, [](uint32_t rva, double percent) { return std::format(" {:X}={:.2f}%", rva, percent); }));
				{
					std::vector<std::pair<const ExportKey*, const ExportBucket*>> exportOrder;
					for (const auto& [key, bucket] : leafExports)
						exportOrder.emplace_back(&key, &bucket);
					std::ranges::sort(exportOrder, [](const auto& lhs, const auto& rhs) { return lhs.second->samples > rhs.second->samples; });
					std::string exportText;
					for (size_t i = 0; i < exportOrder.size() && i < 20; ++i) {
						const auto* bucket = exportOrder[i].second;
						auto topCaller = [](const std::unordered_map<uint32_t, uint32_t>& callers) {
							uint32_t best = 0, bestCount = 0;
							for (const auto& [rva, hits] : callers)
								if (hits > bestCount) {
									best = rva;
									bestCount = hits;
								}
							return std::pair{ best, bestCount };
						};
						const auto [pixlCaller, pixlHits] = topCaller(bucket->pixlCallers);
						const auto [gameCaller, gameHits] = topCaller(bucket->gameCallers);
						exportText += std::format(" {}!{}={:.2f}%(pixl {:X} x{}, game {:X} x{})", ModuleName(exportOrder[i].first->moduleIndex),
							exportOrder[i].first->name, 100.0 * bucket->samples / total, pixlCaller, pixlHits, gameCaller, gameHits);
					}
					logger::info("[PerfProbe]   other DLL functions executing:{}", exportText);
				}
				logger::info("[PerfProbe]   SkyrimSE.exe executing (RVA):{}",
					TopEntries(gameLeaf, 25, total, [](uint32_t rva, double percent) { return std::format(" {:X}={:.2f}%", rva, percent); }));
				logger::info("[PerfProbe]   other DLL <- nearest SkyrimSE.exe site (RVA):{}",
					TopEntries(externalCalls, 25, total, [this](uint64_t key, double percent) {
						return std::format(" {}<-{:X}={:.2f}%", ModuleName(static_cast<size_t>(key >> 32)), static_cast<uint32_t>(key), percent);
					}));

				totalSamples = 0;
				exclusive.fill(0);
				inclusive.fill(0);
				pixlSites.clear();
				pixlSelf.clear();
				leafExports.clear();
				gameLeaf.clear();
				externalCalls.clear();
			}

			std::vector<uintptr_t> stackCopy;
			std::vector<ModuleEntry> modules;
			std::vector<CodeRange> ranges;
			int gameIndex = -1;
			int pixlIndex = -1;
			uint64_t totalSamples = 0;
			std::array<uint32_t, kMaxModules + 1> exclusive{};
			std::array<uint32_t, kMaxModules> inclusive{};
			std::unordered_map<uint32_t, uint32_t> pixlSites;
			std::unordered_map<uint32_t, uint32_t> pixlSelf;
			struct ExportKey
			{
				uint32_t moduleIndex = 0;
				const char* name = nullptr;
				bool operator==(const ExportKey&) const = default;
			};
			struct ExportKeyHash
			{
				size_t operator()(const ExportKey& a_key) const noexcept
				{
					return std::hash<const void*>{}(a_key.name) ^ (static_cast<size_t>(a_key.moduleIndex) * 0x9E3779B97F4A7C15ull);
				}
			};
			struct ExportBucket
			{
				uint32_t samples = 0;
				std::unordered_map<uint32_t, uint32_t> pixlCallers;
				std::unordered_map<uint32_t, uint32_t> gameCallers;
			};
			std::unordered_map<ExportKey, ExportBucket, ExportKeyHash> leafExports;
			std::unordered_map<uint32_t, uint32_t> gameLeaf;
			std::unordered_map<uint64_t, uint32_t> externalCalls;
		};
	}

	void StartSampler()
	{
		// Called by OnFrame whenever the presenting thread changes (the game
		// presents from a different thread at the main menu than in-game).
		const DWORD threadId = GetCurrentThreadId();
		const SampleTarget* previous = currentTarget.load(std::memory_order_acquire);
		if (previous && previous->threadId == threadId)
			return;
		HANDLE renderThread = nullptr;
		if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &renderThread,
				THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
			logger::warn("[PerfProbe] Could not open the render thread for sampling");
			return;
		}
		// The stack between RSP and StackBase is always committed, so copying from
		// RSP upward is safe; the low bound only rejects nonsense RSP values.
		const auto* threadInfo = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
		const auto stackHighLimit = reinterpret_cast<uintptr_t>(threadInfo->StackBase);
		auto* target = new SampleTarget{ renderThread,
			stackHighLimit > (uintptr_t(256) << 20) ? stackHighLimit - (uintptr_t(256) << 20) : 0, stackHighLimit, threadId };
		// Previous targets are intentionally leaked: the sampler may still be using one.
		currentTarget.store(target, std::memory_order_release);
		logger::info("[PerfProbe] Sampling render thread {}", threadId);

		static std::atomic_flag started = ATOMIC_FLAG_INIT;
		if (started.test_and_set())
			return;
		auto* sampler = new RenderThreadSampler();
		std::thread([sampler] { sampler->Run(); }).detach();
	}
}
