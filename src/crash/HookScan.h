#pragma once

// Hooks in the game code.
//
// Many SKSE plugins change the code of the game executable: they write a jump
// to their own code at a place in the game code (a hook). Scan compares the
// code of the game in memory with the game file on the disk, and records each
// jump that goes out of the game executable, and the DLL that it goes to.
//
// The crash report then names:
// - the hooks near the crash place in the game code,
// - the DLL whose hook went to code that is not there any more (an unloaded
//   DLL), when the game jumped into freed memory,
// - the number of hooks of each DLL.
//
// Win32 only: no CommonLibSSE and no game data.

#include <cstdint>

#include "Text.h"

namespace skydiag::crash::hooks
{
	// Normal code (not after a crash): reads the game file (the first time)
	// and records the hooks. Call again from time to time: DLLs add hooks
	// later. Returns the number of hooks.
	int Scan();

	// After a crash (no heap memory): the hooks within 64 bytes before or 16
	// bytes after a_address in the game code. Returns the count written.
	int AppendNear(Out& a_out, std::uintptr_t a_address);

	// After a crash: hooks that go to a_target (or through it). Returns the
	// count written.
	int AppendTo(Out& a_out, std::uintptr_t a_target);

	// After a crash: the number of hooks of each DLL, and the hooks whose
	// target code is not in a loaded module now.
	void AppendSummary(Out& a_out);
}
