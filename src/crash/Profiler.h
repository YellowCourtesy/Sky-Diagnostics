#pragma once

// Sampling profiler for the busy threads of the game.
//
// A helper thread measures the processor use of each thread one time each
// second. It then samples the thread that shows the frames, the threads that
// run SKSE tasks, and the busiest other threads (8 threads at most): about
// 100 times each second it stops each of them for a very short time, reads
// the call stack, and lets the thread continue. Each sample gives three
// facts:
//   - the module that the thread was in (the game, the graphics layer, a
//     system DLL, a mod DLL)
//   - if the thread waited (sleep, lock, frame cap, VSync, other thread)
//   - the nearest mod DLL on the call stack, if there is one
//
// The result is statistical. It shows where the processor time of the game
// goes, by thread and module, and which mod DLLs are involved.

#include <cstddef>
#include <cstdint>

#include "Text.h"

namespace skydiag::profiler
{
	// Starts the profiler. a_renderThreadId is the thread that shows the
	// frames (calls Present). Later calls do nothing.
	bool Start(std::uint32_t a_renderThreadId);

	// Tells the profiler which thread shows the frames. Call it in each
	// frame: it is very cheap when the thread is the same.
	void SetRenderThread(std::uint32_t a_threadId);

	// Tells the profiler that a thread runs SKSE tasks (game update work).
	void NoteTaskThread(std::uint32_t a_threadId);

	struct ThreadInfo
	{
		std::uint32_t id{ 0 };
		bool          isRender{ false };
		bool          isTask{ false };
		std::uint32_t samples{ 0 };
		float         waitShare{ 0 };  // part of the samples in which the thread waited
		float         cpuShare{ 0 };   // processor use from the system counter, 0 to 1
	};

	// The threads that have samples in the last a_windowUs microseconds,
	// least waiting first. Returns the count.
	int ListThreads(ThreadInfo* a_out, int a_max, std::uint64_t a_windowUs);

	// Where a thread was when it did not wait.
	struct Entry
	{
		char  name[80];
		float share;  // part of the samples of the thread, 0 to 1
		bool  isMod;
	};

	// A mod DLL that was on the call stack of a thread.
	struct ModEntry
	{
		char  name[80];
		float onStack;  // part of the samples with this mod on the call stack (nearest mod only)
		float ownCode;  // of these: the thread ran code of the mod DLL itself
		float waiting;  // of these: the thread waited
	};

	// Result for one thread.
	struct Snapshot
	{
		std::uint32_t samples{ 0 };
		float         waitShare{ 0 };
		float         costShare{ 0 };  // part of the time that the thread was stopped by the profiler
		int           count{ 0 };
		Entry         entries[8];      // largest first
		int           modCount{ 0 };
		ModEntry      mods[6];         // largest onStack first
	};

	// a_threadId 0 means the render thread. Safe from all threads.
	void GetSnapshot(Snapshot& a_out, std::uint64_t a_windowUs, std::uint32_t a_threadId = 0);

	// Result for a time range, given as two QueryPerformanceCounter values.
	void GetRange(Snapshot& a_out, std::int64_t a_fromCounter, std::int64_t a_toCounter, std::uint32_t a_threadId = 0);

	// A mod DLL, added up over all sampled threads. The unit is one
	// processor core: 1.0 means that the mod kept one core busy all the time.
	struct ModTotal
	{
		char  name[80];
		float ownCores;      // code of the mod DLL itself
		float waitingCores;  // waits inside calls from the mod
		float totalCores;    // all samples with the mod on the call stack
	};

	// Largest total first. Returns the count.
	int GetMods(ModTotal* a_out, int a_max, std::uint64_t a_windowUs);
	int GetModsRange(ModTotal* a_out, int a_max, std::int64_t a_fromCounter, std::int64_t a_toCounter);

	// The current QueryPerformanceCounter value and its ticks for each
	// second, for callers that give time ranges.
	std::int64_t Now();
	std::int64_t TicksPerSecond();

	// Writes a short text for a snapshot, for example
	// "62% SkyrimSE.exe, 30% waiting; mod on stack: Foo.dll 40%". Returns false if no samples.
	bool Describe(const Snapshot& a_snapshot, char* a_out, std::size_t a_cap);

	// Describes the thread that waited least in a time range. Returns
	// false if there are no samples.
	bool DescribeRange(char* a_out, std::size_t a_cap, std::int64_t a_fromCounter, std::int64_t a_toCounter);

	// Writes the state of the profiler, for fault finding.
	void Status(char* a_out, std::size_t a_cap);

	// Writes a report section for the last 5 seconds. Safe after a crash.
	void Append(Out& a_out);
}
