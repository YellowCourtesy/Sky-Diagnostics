#pragma once

// Monitor: a second process that watches the game from outside.
//
// The game starts it when the tool starts (with SKSE, also from Mod
// Organizer 2): rundll32.exe from Windows runs the function SkyDiagMonitor of
// this DLL. There is no separate program file. The monitor stops when the
// game process ends.
//
// What the monitor does:
// - Writes the minidump of a crash or a freeze from outside the game: this
//   works also when the game process is too damaged to write it.
// - Writes a report when the game ends with an error code and no report was
//   made in the game (fail fast, stack overflow, the handler could not run).
// - Writes a freeze report when the tool's own watchdog thread in the game
//   stops (all of the game is stopped).
// - Adds the Crash Logger log of the same crash to the crash report, if
//   Crash Logger is installed.
// - Opens the crash report after the game closed (setting OpenReport).
//
// Win32 only: no CommonLibSSE and no game data.

#include <cstdint>

#include "Text.h"

namespace skydiag::monitor
{
	struct Options
	{
		const wchar_t* dllPath{ nullptr };    // this DLL; nullptr = the DLL that has this code
		const wchar_t* reportDir{ nullptr };  // reports
		const wchar_t* dataDir{ nullptr };    // session.bin
		const wchar_t* skseDir{ nullptr };    // the SKSE log folder (Crash Logger logs)
		const wchar_t* logPath{ nullptr };    // the log file of the monitor
		std::uint32_t  freezeSeconds{ 30 };
		bool           openReport{ true };
		bool           writeMinidump{ true };
	};

	// In the game: makes the shared memory and starts the monitor process.
	bool Start(const Options& a_options);

	// In the game: true if the monitor process answered in the last seconds.
	bool IsRunning();

	// In the game, each second from the watchdog thread: the game process is alive.
	void Beat();

	// In the game: the game update thread (main thread) id, for the freeze report.
	void SetMainThread(std::uint32_t a_threadId);

	// In the game: a loading screen is open.
	void SetLoading(bool a_loading);

	// In the game: the player chose to quit (console qqq, menu Quit).
	void NoteQuit();

	// In the game: a request to close the window (Alt+F4, the X button, a close
	// message from a different program).
	void NoteCloseRequest();

	// In the game: watches the game window for close requests (Alt+F4, the X
	// button, a close message) and calls NoteCloseRequest. Call when the game
	// window exists. Returns false if the window was not found.
	bool WatchWindowClose();

	// True after NoteQuit, or within 10 seconds after NoteCloseRequest. A crash
	// then is a crash while the game closes (a forced close), not a crash in
	// play. Safe after a crash.
	bool IsClosing();

	// After a crash or in a stall report: asks the monitor to write the
	// minidump. a_pointers: the EXCEPTION_POINTERS of the crash in this
	// process, or nullptr for a freeze. Waits at most a_timeoutMs. Returns
	// true if the monitor wrote the file. Safe after a crash (no heap memory).
	bool RequestDump(const wchar_t* a_path, const void* a_pointers, std::uint32_t a_threadId, std::uint32_t a_timeoutMs);

	// A crash report was written to a_path. a_open: open it after the game
	// closed (if the setting is on). Safe after a crash.
	void NoteReport(const wchar_t* a_path, bool a_open, std::uint64_t a_crashFileTime);

	// Writes the wait chain of a thread (Windows Wait Chain Traversal): what
	// it waits for, and which thread holds that. a_owner gets the first other
	// thread in the chain, or 0. Works in the game and in the monitor.
	void AppendWaitChain(Out& a_out, std::uint32_t a_threadId, std::uint32_t* a_owner);

	// Writes the visible dialog windows (error messages) of a process. Returns
	// the count. Sends no window messages, so a frozen window does not stop it.
	int AppendDialogs(Out& a_out, std::uint32_t a_processId);

	// The entry of the monitor process (SkyDiagMonitor in main.cpp calls it).
	// a_commandLine: the process id of the game.
	void Run(const char* a_commandLine);
}
