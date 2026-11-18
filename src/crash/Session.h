#pragma once

// Session record.
//
// A small memory-mapped file holds the state of the session, the last
// events and one memory sample for each second. The operating system keeps
// the file content when the process stops for any reason. At the next start,
// the tool reads the file. If the session did not end correctly and no crash
// report was made, the tool makes an "unclean exit" report from it.
//
// Win32 only: no CommonLibSSE and no game data.

#include <cstddef>
#include <cstdint>

#include "Text.h"

namespace skydiag::session
{
	struct Config
	{
		const wchar_t* dataDir{ nullptr };    // folder for session.bin, must exist
		const wchar_t* reportDir{ nullptr };  // folder for reports, must exist
		const char*    toolVersion{ "" };
		const char*    gameVersion{ "" };
		const char*    platform{ "" };
	};

	// Reads the record of the previous session, makes the unclean exit
	// report if necessary, then starts a new record. Returns false if the
	// record could not be made. a_previousUnclean is set to true if a
	// report was written.
	bool Start(const Config& a_config, bool* a_previousUnclean);

	// For the monitor process, after the game stopped: if the session record
	// in a_dataDir shows a running session (no correct exit, no crash report),
	// writes the unclean exit report with the exit code and marks the record,
	// so the next game start does not write it again. Returns true if a report
	// was written; a_pathOut gets its path.
	bool WriteExitReport(const wchar_t* a_dataDir, const wchar_t* a_reportDir, std::uint32_t a_exitCode, bool a_closing, wchar_t* a_pathOut,
		std::size_t a_pathCap);

	// Adds one line to the event list. Safe from all threads.
	void Event(const char* a_fmt, ...);

	// A function that gets the text of each event (for the session
	// history). It is not called after a crash.
	void SetEventObserver(void (*a_observer)(const char* a_text));

	// Values that the caller measures and gives to Sample.
	struct SampleInput
	{
		std::uint32_t updateResponseMs{ 0 };  // response time of the game update thread
		float         fps{ 0 };               // average of the last seconds, 0 = not known
		float         worstFrameMs{ 0 };      // slowest frame of the last seconds
		std::uint32_t frameLimit{ 0 };        // frames::Limit value
		bool          loading{ false };       // a loading screen is open
	};

	// Takes one sample (memory and the given values). Call one time each
	// second from one thread.
	void Sample(const SampleInput& a_input);

	// The newest sample, for the overlay.
	struct Latest
	{
		std::uint32_t updateResponseMs{ 0 };
		std::uint64_t commit{ 0 };
		std::uint64_t systemCommitFree{ 0 };
	};
	bool GetLatest(Latest& a_out);

	// Tells the session if a loading screen is open.
	void SetLoading(bool a_loading);

	// Seconds since the last loading screen closed. 0 while one is open. A
	// large value if there was none.
	float SecondsSinceLoading();

	// Change of the game memory in the newest period that has no loading
	// screen (at most 10 minutes). a_recentMbPerMinute is the change in the
	// last 2 minutes only: memory that grew before but is stable now is not
	// a leak that continues. Returns false if the period is shorter than
	// 4 minutes.
	bool GetMemoryTrend(float& a_mbPerMinute, float& a_minutes, float& a_recentMbPerMinute);

	// For tests: private memory of the process from the address space scan.
	std::uint64_t PrivateMemoryByScanForTest();

	// State changes.
	void NoteCleanExit();
	void NoteCrashReported();
	void NoteStall();

	// Writes the last events and the memory data into a report. Safe after
	// a crash.
	void AppendRecent(Out& a_out);

	// If the last sample shows that memory was almost full, writes one
	// sentence about it and returns true.
	bool AppendMemoryWarning(Out& a_out);
}
