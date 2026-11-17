#pragma once

#include <array>
#include <atomic>
#include <cstdint>

/**
 * Always-on CPU frame breakdown for diagnosing low frame rates.
 *
 * Accumulates wall-clock time spent in PIXL's per-draw hooks, the present
 * path and each module's per-frame callbacks, and writes a one-line summary
 * to PIXLRenderer.log every few seconds ([PerfProbe]). It only reads timers;
 * it never changes rendering.
 */
namespace PerfProbe
{
	enum class Bucket : uint32_t
	{
		PresentHook,           // whole IDXGISwapChain::Present detour
		FrameLimiter,          // PIXL limiter + frame-latency wait
		DX12Present,           // whole D3D12 compatibility-swapchain present
		DX12AllocatorWait,     // CPU wait for the previous D3D12 submission
		DX12SwapPresent,       // the real D3D12 swap-chain Present call
		BeginTechnique,        // whole BSShader::BeginTechnique detour
		BeginTechniqueNative,  // Skyrim's own BeginTechnique inside it
		DirtyStateNative,      // Skyrim's state flush
		DirtyStatePixl,        // PIXL State::Draw per flush
		PointLights,           // PIXL point-light setup detour (incl. native)
		Count
	};

	inline constexpr std::array<const char*, static_cast<size_t>(Bucket::Count)> kBucketNames{
		"present", "limiter", "dx12Present", "dx12AllocWait", "dx12SwapPresent",
		"beginTech", "beginTechNative", "flushNative", "flushPixl", "pointLights"
	};

	inline std::array<std::atomic<int64_t>, static_cast<size_t>(Bucket::Count)> bucketTicks{};
	inline std::atomic<uint64_t> drawFlushes{ 0 };

	inline int64_t Now()
	{
		LARGE_INTEGER probeNow{};
		QueryPerformanceCounter(&probeNow);
		return probeNow.QuadPart;
	}

	inline void Add(Bucket bucket, int64_t ticks)
	{
		bucketTicks[static_cast<size_t>(bucket)].fetch_add(ticks, std::memory_order_relaxed);
	}

	struct Scope
	{
		explicit Scope(Bucket a_bucket) :
			bucket(a_bucket), start(Now()) {}
		~Scope() { Add(bucket, Now() - start); }
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

		Bucket bucket;
		int64_t start;
	};

	/** Called once per frame at the top of Present; logs a summary every few seconds. */
	void OnFrame();
}
