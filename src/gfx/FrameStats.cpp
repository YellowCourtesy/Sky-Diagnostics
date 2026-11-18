#include "FrameStats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace skydiag::frames
{
	namespace
	{
		constexpr std::uint32_t kCapacity = 4096;
		constexpr std::uint64_t kWindowUs = 5'000'000;
		constexpr std::uint64_t kRefreshUs = 250'000;

		Frame         g_ring[kCapacity];
		std::uint32_t g_count = 0;  // frames added, total
		std::uint64_t g_sinceRefreshUs = 0;

		std::mutex g_latestLock;
		Summary    g_latest;

		using Clock = std::chrono::steady_clock;
		std::atomic<Clock::rep> g_lastAdd{ 0 };
	}

	Summary Compute(std::uint64_t a_windowUs)
	{
		static float times[kCapacity];

		Summary       result;
		std::uint64_t totalUs = 0;
		std::uint64_t presentUs = 0;
		std::uint64_t queue = 0;
		std::uint64_t cpuUs = 0;
		std::uint64_t inputUs = 0;
		std::uint32_t inputFrames = 0;
		std::uint32_t n = 0;

		const std::uint32_t stored = g_count < kCapacity ? g_count : kCapacity;
		for (std::uint32_t i = 0; i < stored && totalUs < a_windowUs; ++i) {
			const Frame& f = g_ring[(g_count - 1 - i) % kCapacity];
			totalUs += f.frameUs;
			presentUs += f.presentUs;
			queue += f.gpuQueue;
			cpuUs += f.cpuUs;
			if (f.inputUs != 0) {
				inputUs += f.inputUs;
				++inputFrames;
			}
			times[n++] = static_cast<float>(f.frameUs) / 1000.0f;
		}
		if (n == 0 || totalUs == 0) {
			return result;
		}

		result.frames = n;
		result.averageMs = static_cast<float>(totalUs) / 1000.0f / static_cast<float>(n);
		result.fps = 1000.0f / result.averageMs;
		result.presentMs = static_cast<float>(presentUs) / 1000.0f / static_cast<float>(n);
		result.presentShare = result.presentMs / result.averageMs;
		result.gpuQueue = static_cast<float>(queue) / static_cast<float>(n);
		result.worstMs = *std::max_element(times, times + n);
		if (cpuUs > 0) {
			result.busyShare = static_cast<float>(cpuUs) / static_cast<float>(totalUs);
		}

		double squares = 0.0;
		for (std::uint32_t i = 0; i < n; ++i) {
			const double d = static_cast<double>(times[i]) - static_cast<double>(result.averageMs);
			squares += d * d;
		}
		result.jitter = static_cast<float>(std::sqrt(squares / n)) / result.averageMs;

		// The game reads the input at the start of a frame. The frame is on
		// the GPU one frame time later, and the GPU completes it after the
		// frames that are in its queue.
		result.latencyMs = result.averageMs * (1.0f + result.gpuQueue);
		// If the time of the input read is known for most frames, use it:
		// input read to the end of Present, plus the frames in the GPU queue.
		if (inputFrames * 2 >= n) {
			result.latencyMs = static_cast<float>(inputUs) / 1000.0f / static_cast<float>(inputFrames) + result.averageMs * result.gpuQueue;
			result.latencyMeasured = true;
		}

		const std::uint32_t index = (n * 99) / 100;
		std::nth_element(times, times + index, times + n);
		result.low1Ms = times[index < n ? index : n - 1];

		// How many frames have almost the same time. Some slow frames
		// (stutter) do not change this value much, different from the
		// standard deviation.
		std::nth_element(times, times + n / 2, times + n);
		const float   median = times[n / 2];
		std::uint32_t regular = 0;
		for (std::uint32_t i = 0; i < n; ++i) {
			if (times[i] >= median * 0.94f && times[i] <= median * 1.06f) {
				++regular;
			}
		}
		result.regularShare = static_cast<float>(regular) / static_cast<float>(n);

		// The GPU is the limit if it has not completed the frame before
		// when the game submits the next one. If the GPU keeps up, a limiter
		// holds the game back in one of two ways: the game waits in Present
		// (VSync), or the render thread is idle for part of each frame and
		// the frame time is very regular (a frame cap in the game or in a
		// tool). In all other cases the processor work sets the rate.
		if (n < 20) {
			result.limit = Limit::kUnknown;
		} else if (result.gpuQueue >= 0.75f) {
			result.limit = Limit::kGpu;
		} else if (result.presentShare >= 0.25f) {
			result.limit = Limit::kLimiter;
		} else if (result.regularShare >= 0.85f) {
			// A constant frame time. Processor work always changes from
			// frame to frame, so a cap sets this frame time. Some caps keep
			// the thread busy while they wait, so the busy share cannot be
			// used here.
			result.limit = Limit::kLimiter;
		} else if (result.busyShare >= 0.0f && result.busyShare < 0.8f && result.jitter < 0.08f) {
			result.limit = Limit::kLimiter;
		} else {
			result.limit = Limit::kCpu;
		}
		return result;
	}

	void Add(const Frame& a_frame)
	{
		g_ring[g_count % kCapacity] = a_frame;
		++g_count;
		g_lastAdd = Clock::now().time_since_epoch().count();

		g_sinceRefreshUs += a_frame.frameUs;
		if (g_sinceRefreshUs >= kRefreshUs) {
			g_sinceRefreshUs = 0;
			const Summary summary = Compute(kWindowUs);
			const std::scoped_lock lock{ g_latestLock };
			g_latest = summary;
		}
	}

	Summary Latest()
	{
		// The game shows no frames while its window is not active. Old
		// values must not look like current values.
		const auto idle = Clock::now().time_since_epoch() - Clock::duration(g_lastAdd.load());
		if (idle > std::chrono::seconds(2)) {
			return {};
		}
		const std::scoped_lock lock{ g_latestLock };
		return g_latest;
	}

	int History(float* a_out, int a_max)
	{
		const std::uint32_t stored = g_count < kCapacity ? g_count : kCapacity;
		const std::uint32_t n = stored < static_cast<std::uint32_t>(a_max) ? stored : static_cast<std::uint32_t>(a_max);
		for (std::uint32_t i = 0; i < n; ++i) {
			a_out[i] = static_cast<float>(g_ring[(g_count - n + i) % kCapacity].frameUs) / 1000.0f;
		}
		return static_cast<int>(n);
	}

	const char* LimitText(Limit a_limit)
	{
		switch (a_limit) {
		case Limit::kCpu: return "CPU (game update or render submit)";
		case Limit::kGpu: return "GPU (the GPU is one frame or more behind)";
		case Limit::kLimiter: return "VSync or frame cap (the frame time is held constant)";
		default: return "not known yet";
		}
	}
}
