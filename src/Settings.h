#pragma once

// Settings from the [[Settings]] part of Data/SKSE/Plugins/SkyrimDiagnostics.ini.
// Standard C++ only.

#include <string>

namespace skydiag
{
	struct Settings
	{
		int   view{ 0 };                  // 0 = all, 1 = warnings only, 2 = off
		int   toggleKey{ 0x79 };          // Windows virtual key code, 0x79 = F10
		float warningHoldSeconds{ 30.0f };
		float fpsBelow{ 57.0f };
		bool  profiler{ true };
		bool  papyrus{ true };
		bool  engineLimits{ true };
		bool  animations{ true };         // count the loaded animation files
		bool  memoryByDll{ true };        // count the heap memory of each mod DLL
		bool  papyrusTime{ true };        // measure the time in each script function
		int   stallSeconds{ 30 };
		bool  fileFound{ false };
	};

	// Reads the file. Keys that are missing or not valid keep their default.
	Settings LoadSettings(const std::string& a_path);

	// Reads settings from text (used by LoadSettings and by the tests).
	Settings ParseSettings(const std::string& a_text);
}
