#pragma once

// The system: processor, memory, GPU and driver, operating system, and on
// Linux the Wine/Proton version, DXVK and the Wine, Proton and DXVK settings
// (environment variables). Also the SKSE plugins that SKSE did not load.
//
// Win32 only: no CommonLibSSE and no game data.

#include <string>
#include <vector>

namespace skydiag::sysinfo
{
	// Normal code: collects the data. Call one time, after the game window
	// exists (the GPU data needs DXGI). Returns the text (also kept for the
	// SYSTEM section of the crash and stall reports).
	const std::string& Collect();

	// The text from Collect, or "" before it. Safe after a crash (the text
	// does not change after Collect).
	const char* Text();

	// True if a DLL with this file name is loaded in this process.
	bool IsLoaded(const wchar_t* a_name);

	// True if d3d11.dll of this process is DXVK (Direct3D 11 on Vulkan).
	bool IsDxvk();

	// Reads the SKSE log (skse64.log or sksevr.log in a_skseLogDir) and returns
	// the lines about plugins that SKSE did not load, or that it reported as
	// not compatible. Normal code.
	std::vector<std::string> SksePluginProblems(const std::wstring& a_skseLogDir);
}
