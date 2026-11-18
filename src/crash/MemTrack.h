#pragma once

// Heap memory by mod DLL.
//
// The import tables of the mod DLLs are changed so that their calls to the
// heap functions (HeapAlloc, HeapReAlloc, HeapFree, malloc, calloc, realloc,
// free) go through small counting functions. Each DLL has its own entry
// code that gives the number of the DLL to the counting function. The
// counting function adds the size and calls the real function.
//
// Limits of the method:
// - Memory that a DLL gets from the game's own memory manager is not
//   counted (many SKSE plugins use it for game objects).
// - VirtualAlloc and memory of the graphics driver are not counted.
// - Memory that one DLL allocates and a different module releases stays in
//   the count of the first DLL.
// - Memory that a DLL allocated before the tool started is not known. If
//   the DLL releases it later, its count goes below zero.

#include <cstdint>

#include "Text.h"

namespace skydiag::memtrack
{
	// Changes the import tables of the mod DLLs that are loaded now and
	// were not changed before. Call it again after more DLLs are loaded.
	// Not from a crash context. Returns the number of tracked DLLs.
	int HookModules();

	struct Entry
	{
		char          name[96];
		std::int64_t  inUseBytes;      // allocated minus released
		std::uint64_t allocatedBytes;  // all allocations since the start
		std::uint64_t allocations;     // number of allocations
	};

	// Largest memory in use first. Returns the count. No heap, no lock.
	int GetTop(Entry* a_out, int a_max);

	// Section for crash, stall and unclean exit reports.
	void Append(Out& a_out);
}
