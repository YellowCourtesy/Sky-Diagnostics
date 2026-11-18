#pragma once

// Load times of the game start and of save loads.
//
// - SKSE plugins: the time of the load function of each plugin
//   (SKSEPlugin_Load). The tool changes the GetProcAddress import of SKSE, so
//   that SKSE calls the load function through a timer. On Skyrim AE this
//   covers all plugins (the tool starts in the preload step); on SE and VR
//   only the plugins that load after this tool.
// - SKSE messages: the time of the message function of each plugin for each
//   message (PostLoad, DataLoaded, PreLoadGame, PostLoadGame and the
//   others), and the time of the co-save load and revert functions. The tool
//   changes the RegisterListener and SetLoadCallback functions of the SKSE
//   interfaces; only functions that plugins give after that are timed.
// - Plugin files (ESP, ESM, ESL): the time from the open of each file to the
//   open of the next one. Not usable: the test of 0.2.0 gave all the time to
//   one file, so the session report does not show it. The CreateFile and
//   ReadFile imports of the game are still changed for the slow file reads.
//
// The functions that the game calls through these changes only measure time:
// they call the original function at once and change nothing.
//
// Win32 only: no CommonLibSSE and no game data.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace skydiag::loadtimes
{
	// Call as early as possible (the first SKSE entry point).
	void HookPluginLoad();  // SKSE's GetProcAddress
	void HookFileOpen();    // CreateFileA and CreateFileW of the game executable

	// a_messaging: the SKSE messaging interface. a_serialization: the SKSE
	// serialization interface (or nullptr). Call in SKSEPlugin_Load.
	void HookInterfaces(const void* a_messaging, const void* a_serialization);

	// Marks the end of the data load (SKSE message DataLoaded): the last
	// plugin file gets its time.
	void NoteDataLoaded();

	struct Row
	{
		std::string module;   // the DLL, or the plugin file
		std::string what;     // "SKSEPlugin_Load", "message DataLoaded", "co-save load", "plugin file"
		double      totalMs{ 0 };
		double      maxMs{ 0 };
		int         count{ 0 };
	};

	// File reads and opens of the game that took 8 ms or more (ReadFile and
	// CreateFile imports of the game executable), with the file name. Writes
	// those that ended between the counter values a_from and a_to
	// (QueryPerformanceCounter), at most 5, as "name 35 ms, name 12 ms".
	// Returns the count.
	int SlowFileReads(long long a_from, long long a_to, char* a_out, std::size_t a_cap);

	// All measured times, the largest first. Normal code only.
	std::vector<Row> Rows();

	// The SKSE message name of a message type.
	const char* MessageName(std::uint32_t a_type);
}
