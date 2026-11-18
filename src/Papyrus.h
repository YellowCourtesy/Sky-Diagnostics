#pragma once

// Papyrus script sampler.
//
// A helper thread looks at the list of script stacks of the Papyrus virtual
// machine 20 times each second. It only reads memory: it takes no lock and
// writes nothing to the game. For each stack it records the script of the
// top function and if the stack runs or waits.
//
// The result is statistical. It shows which scripts have work for the
// virtual machine, not exact times.

#include <cstdint>

namespace skydiag::papyrus
{
	// Starts the sampler. Call after the game data is loaded.
	// a_measureTime: also measure the time in each script function. For
	// this, one entry in the function table of the script function class
	// is changed, so that each run of script code goes through a function
	// that reads the clock before and after.
	void Start(bool a_measureTime);

	struct Entry
	{
		char  name[64];
		float running;  // average number of stacks that run or want to run
		float waiting;  // average number of stacks that wait (timer, other script, latent function)
	};

	// A function that was at the top of stacks that run.
	struct FunctionEntry
	{
		char  name[96];  // "Script.Function"
		float running;   // average number of stacks that run in this function
	};

	// Time that the virtual machine used in a script function (measured).
	struct TimedEntry
	{
		char  name[96];        // "Script.Function"
		float msPerSecond;     // milliseconds of script work in each second
		float callsPerSecond;  // how many times the virtual machine ran code of the function
	};

	struct Snapshot
	{
		bool          valid{ false };
		std::uint32_t samples{ 0 };
		float         runningStacks{ 0 };  // average
		float         waitingStacks{ 0 };  // average
		std::uint32_t maxStacks{ 0 };
		int           count{ 0 };
		Entry         entries[8];  // most running stacks first
		int           functionCount{ 0 };
		FunctionEntry functions[8];  // most running stacks first
		int           timedCount{ 0 };
		TimedEntry    timed[8];  // most time first. Empty if the time measurement is off
	};

	// Result of the last full period (about 3 seconds). Safe from all threads.
	void GetSnapshot(Snapshot& a_out);

	// Text that says if the sampler works, for fault finding.
	const char* Status();
}
