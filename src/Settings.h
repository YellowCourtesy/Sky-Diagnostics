#pragma once

// Settings from the [[Settings]] part of Data/SKSE/Plugins/SkyrimDiagnostics.ini.
// Standard C++ only.

#include <string>

namespace skydiag
{
	struct Settings
	{
		float warningHoldSeconds{ 30.0f };
		float fpsBelow{ 57.0f };
		bool  profiler{ true };
		bool  papyrus{ true };
		bool  engineLimits{ true };
		bool  animations{ true };         // count the loaded animation files
		bool  memoryByDll{ true };        // count the heap memory of each mod DLL
		bool  papyrusTime{ true };        // measure the time in each script function
		bool  papyrusLatency{ true };     // script latency test each 5 seconds
		int   stallSeconds{ 30 };
		int   loadingScreenSeconds{ 120 }; // a loading screen open longer, with no disk reads for 30 s: infinite loading screen report
		bool  monitor{ true };           // start the monitor process (rundll32.exe with this DLL)
		bool  openReport{ true };        // open the crash report after the game closed
		bool  fileFound{ false };
	};

	// Reads the file. Keys that are missing or not valid keep their default.
	Settings LoadSettings(const std::string& a_path);

	// Reads settings from text (used by LoadSettings and by the tests).
	Settings ParseSettings(const std::string& a_text);
}
