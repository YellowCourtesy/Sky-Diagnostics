#pragma once

// Frame time statistics. Standard C++ only: no game, no graphics API.

#include <cstdint>

namespace skydiag::frames
{
	struct Frame
	{
		std::uint32_t frameUs{ 0 };    // time from the start of one Present call to the start of the next
		std::uint32_t presentUs{ 0 };  // time that the Present call took
		std::uint32_t gpuQueue{ 0 };   // older frames that the GPU did not complete when this frame was submitted
		std::uint32_t cpuUs{ 0 };      // processor time that the render thread used in this frame (coarse)
		std::uint32_t inputUs{ 0 };    // time from the input read of this frame to the end of its Present call, 0 = not known
	};

	// What limits the frame rate.
	enum class Limit : std::uint32_t
	{
		kUnknown = 0,
		kCpu = 1,      // the game update or the render submit
		kGpu = 2,      // the GPU is behind
		kLimiter = 3,  // VSync or a frame cap holds the frame time constant
	};

	struct Summary
	{
		std::uint32_t frames{ 0 };
		float         fps{ 0 };
		float         averageMs{ 0 };
		float         low1Ms{ 0 };        // 99 % of the frames are faster than this
		float         worstMs{ 0 };
		float         presentMs{ 0 };     // average time in Present
		float         presentShare{ 0 };  // presentMs / averageMs
		float         gpuQueue{ 0 };      // average
		float         busyShare{ -1 };    // part of the frame time in which the render thread used the processor; -1 = not known
		float         jitter{ 0 };        // standard deviation of the frame time / average
		float         regularShare{ 0 };  // part of the frames with a time within 6 % of the median
		float         latencyMs{ 0 };     // input read to GPU done (display not included)
		bool          latencyMeasured{ false };  // true: from the time of the input read. false: estimate from the frame time
		Limit         limit{ Limit::kUnknown };
	};

	// Adds one frame. Call from the render thread only. The summary is
	// calculated again four times each second.
	void Add(const Frame& a_frame);

	// The last calculated summary (covers about 5 seconds). Safe from all
	// threads. Empty if no frame was shown in the last 2 seconds.
	Summary Latest();

	// Calculates the summary of the last a_windowUs microseconds now.
	// Render thread or tests only.
	Summary Compute(std::uint64_t a_windowUs);

	// Copies the newest frame times (milliseconds, oldest first) for a
	// graph. Render thread only. Returns the number of values.
	int History(float* a_out, int a_max);

	const char* LimitText(Limit a_limit);
}
