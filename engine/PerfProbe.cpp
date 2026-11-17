#include "PerfProbe.h"

#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "Globals.h"
#include "Profiler.h"
#include "RenderModule.h"
#include "ShaderCache.h"

namespace PerfProbe
{
	namespace
	{
		constexpr double kReportSeconds = 5.0;

		uint64_t FileTimeTicks(const FILETIME& a_time)
		{
			return (static_cast<uint64_t>(a_time.dwHighDateTime) << 32) | a_time.dwLowDateTime;
		}

		// 100 ns units of CPU time used by the calling thread / whole process.
		uint64_t ThreadCpu100ns()
		{
			FILETIME created{}, exited{}, kernel{}, user{};
			if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
				return 0;
			return FileTimeTicks(kernel) + FileTimeTicks(user);
		}

		uint64_t ProcessCpu100ns()
		{
			FILETIME created{}, exited{}, kernel{}, user{};
			if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
				return 0;
			return FileTimeTicks(kernel) + FileTimeTicks(user);
		}

		struct WindowState
		{
			int64_t frequency = 0;
			int64_t windowStart = 0;
			int64_t previousFrame = 0;
			uint64_t frames = 0;
			double worstFrameMs = 0.0;
			uint64_t threadCpuStart = 0;
			uint64_t processCpuStart = 0;
			double gpuPassSumMs = 0.0;
			double cpuPassSumMs = 0.0;
		};
	}

	void OnFrame()
	{
		static WindowState probeWindow{};
		static DWORD presentThreadId = 0;
		const int64_t probeNow = Now();
		if (GetCurrentThreadId() != presentThreadId) {
			// Presenting moved to another thread: restart the window so the
			// per-thread CPU numbers stay meaningful, and retarget the sampler.
			presentThreadId = GetCurrentThreadId();
			probeWindow.windowStart = 0;
		}
		if (probeWindow.frequency == 0) {
			LARGE_INTEGER probeFrequency{};
			QueryPerformanceFrequency(&probeFrequency);
			probeWindow.frequency = probeFrequency.QuadPart > 0 ? probeFrequency.QuadPart : 1;
		}
		const double ticksToMs = 1000.0 / static_cast<double>(probeWindow.frequency);

		if (probeWindow.windowStart == 0) {
			probeWindow.windowStart = probeNow;
			probeWindow.previousFrame = probeNow;
			probeWindow.threadCpuStart = ThreadCpu100ns();
			probeWindow.processCpuStart = ProcessCpu100ns();
			for (auto& ticks : bucketTicks)
				ticks.store(0, std::memory_order_relaxed);
			drawFlushes.store(0, std::memory_order_relaxed);
			{
				std::lock_guard lock(moduleTicksMutex);
				moduleTicks.clear();
			}
			StartSampler();
			logger::info("[PerfProbe] CPU frame breakdown enabled; a summary is logged every {:.0f}s", kReportSeconds);
			return;
		}

		++probeWindow.frames;
		probeWindow.worstFrameMs = std::max(probeWindow.worstFrameMs,
			static_cast<double>(probeNow - probeWindow.previousFrame) * ticksToMs);
		probeWindow.previousFrame = probeNow;
		if (globals::profiler) {
			probeWindow.gpuPassSumMs += globals::profiler->GetTotalTimeMs();
			probeWindow.cpuPassSumMs += globals::profiler->GetCpuTotalTimeMs();
		}

		const double windowMs = static_cast<double>(probeNow - probeWindow.windowStart) * ticksToMs;
		if (windowMs < kReportSeconds * 1000.0 || probeWindow.frames == 0)
			return;

		const double probeFrames = static_cast<double>(probeWindow.frames);
		const double frameMs = windowMs / probeFrames;
		const uint64_t threadCpu = ThreadCpu100ns();
		const uint64_t processCpu = ProcessCpu100ns();
		const double renderThreadBusyMs = threadCpu >= probeWindow.threadCpuStart ?
		                                      static_cast<double>(threadCpu - probeWindow.threadCpuStart) / 10000.0 / probeFrames : 0.0;
		const double processCores = static_cast<double>(processCpu - probeWindow.processCpuStart) / 10000.0 / windowMs;

		auto perFrame = [&](Bucket bucket) {
			return static_cast<double>(bucketTicks[static_cast<size_t>(bucket)].exchange(0, std::memory_order_relaxed)) *
			       ticksToMs / probeFrames;
		};
		std::string probeHooks;
		for (size_t i = 0; i < static_cast<size_t>(Bucket::Count); ++i)
			probeHooks += std::format(" {}={:.2f}", kBucketNames[i], perFrame(static_cast<Bucket>(i)));

		std::vector<std::pair<double, RenderModule*>> probeModuleTimes;
		{
			std::lock_guard lock(moduleTicksMutex);
			for (auto* renderModule : RenderModule::GetModuleList()) {
				const auto found = moduleTicks.find(renderModule);
				if (found != moduleTicks.end() && found->second > 0)
					probeModuleTimes.emplace_back(static_cast<double>(found->second) * ticksToMs / probeFrames, renderModule);
			}
			moduleTicks.clear();
		}
		std::ranges::sort(probeModuleTimes, [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
		double moduleTotalMs = 0.0;
		std::string probeModules;
		for (size_t i = 0; i < probeModuleTimes.size(); ++i) {
			moduleTotalMs += probeModuleTimes[i].first;
			if (i < 8)
				probeModules += std::format(" {}={:.2f}", probeModuleTimes[i].second->GetShortName(), probeModuleTimes[i].first);
		}

		std::string probePasses;
		if (globals::profiler) {
			std::vector<const Profiler::TimerResult*> probeSorted;
			for (const auto& result : globals::profiler->GetResults())
				if (result.valid)
					probeSorted.push_back(&result);
			std::ranges::sort(probeSorted, [](const auto* lhs, const auto* rhs) {
				return lhs->avgMs + lhs->cpuAvgMs > rhs->avgMs + rhs->cpuAvgMs;
			});
			for (size_t i = 0; i < probeSorted.size() && i < 8; ++i)
				probePasses += std::format(" {}=gpu{:.2f}/cpu{:.2f}", probeSorted[i]->name, probeSorted[i]->avgMs, probeSorted[i]->cpuAvgMs);
		}

		const bool probeCompiling = globals::shaderCache && globals::shaderCache->IsCompiling();
		logger::info(
			"[PerfProbe] {:.1f} fps | frame {:.2f} ms avg, worst {:.1f} ms | render thread CPU busy {:.2f} ms/frame ({:.0f}%) | "
			"process CPU {:.1f} cores | draw flushes {:.0f}/frame | shaders compiling: {}",
			1000.0 / frameMs, frameMs, probeWindow.worstFrameMs, renderThreadBusyMs,
			100.0 * renderThreadBusyMs / frameMs, processCores,
			static_cast<double>(drawFlushes.exchange(0, std::memory_order_relaxed)) / probeFrames,
			probeCompiling ? "yes" : "no");
		logger::info("[PerfProbe]   PIXL hooks ms/frame:{}", probeHooks);
		logger::info("[PerfProbe]   PIXL modules CPU ms/frame (total {:.2f}):{}", moduleTotalMs, probeModules);
		logger::info("[PerfProbe]   PIXL passes ms (GPU total {:.2f}, CPU total {:.2f}):{}",
			probeWindow.gpuPassSumMs / probeFrames, probeWindow.cpuPassSumMs / probeFrames, probePasses);

		probeWindow.windowStart = probeNow;
		probeWindow.frames = 0;
		probeWindow.worstFrameMs = 0.0;
		probeWindow.threadCpuStart = threadCpu;
		probeWindow.processCpuStart = processCpu;
		probeWindow.gpuPassSumMs = 0.0;
		probeWindow.cpuPassSumMs = 0.0;
	}
}
