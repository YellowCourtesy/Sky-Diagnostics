#pragma once

// GPU time of each render pass.
//
// A render pass here is the work between two changes of the render targets
// (OMSetRenderTargets). At each change, the tool adds a timestamp query to
// the GPU command list. Three frames later it reads the results: the GPU time
// of each pass, and of the whole frame.
//
// A pass is named by its render target: size and format (for example
// "1920x1080 R11G11B10_FLOAT"), or "depth only 2048x2048". The same name can
// be several passes of a frame: their times are added. The names of the
// game's passes (shadows, water and the like) are not known to the tool.
//
// Win32 and Direct3D 11 only.

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace skydiag::gpu
{
	// Starts the measurement (the render thread, in the first Present call).
	bool Start(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

	// At the start of each Present call (the render thread).
	void OnPresent();

	struct Pass
	{
		char  name[64];
		float ms;       // GPU time each frame, average of the last second
		int   count;    // passes with this name in a frame
	};

	struct Snapshot
	{
		bool  valid{ false };
		float frameMs{ 0 };   // GPU time of the whole frame, average of the last second
		int   passCount{ 0 };
		Pass  passes[12];     // most time first
	};

	// Safe from all threads.
	void GetSnapshot(Snapshot& a_out);

	// Session values for the report: a_out gets up to a_max passes with the
	// most average time over the session, and their largest one-second value.
	struct SessionPass
	{
		char  name[64];
		float averageMs;
		float maxMs;
	};
	int GetSession(SessionPass* a_out, int a_max, float& a_frameAverageMs, float& a_frameMaxMs);
}
