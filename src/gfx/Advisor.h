#pragma once

// Live warnings.
//
// Takes the current measurements and gives a short list of warnings, each
// with the most probable cause. Standard C++ only: testable without the game.

#include <cstdint>

#include "FrameStats.h"

namespace skydiag::advisor
{
	// Profiler result for one sampled thread.
	struct ThreadProfile
	{
		bool  isRender{ false };       // shows the frames
		bool  isTask{ false };         // runs SKSE tasks (game update work)
		float waitShare{ 0 };          // part of the time that the thread waits
		char  topModule[80]{};         // module in which the thread runs most (not waiting)
		float topModuleShare{ 0 };
		bool  topModuleIsGame{ false };
	};

	// A mod DLL, added up over all sampled threads. 1.0 = one processor core.
	struct ModLoad
	{
		char  name[80]{};
		float ownCores{ 0 };      // code of the mod DLL itself
		float waitingCores{ 0 };  // waits inside calls from the mod
		float totalCores{ 0 };    // all samples with the mod on the call stack
	};

	struct Inputs
	{
		frames::Summary frames;

		// False during a loading screen and for a short time after it, and
		// for the first 10 seconds after the game window shows. The frame,
		// stutter, latency and Papyrus rules are off then: a load has slow
		// frames and many script stacks, and that is normal.
		bool settled{ true };

		// The frame rate warning shows below this value.
		float fpsBelow{ 57.0f };

		// What the busiest thread did in the last slow frame, if one
		// occurred in the last 30 seconds. Empty if not known.
		char stutterCause[120]{};

		// Profiler.
		ThreadProfile threads[8];
		int           threadCount{ 0 };
		ModLoad       mods[4];
		int           modCount{ 0 };

		// Papyrus.
		bool          hasPapyrus{ false };
		float         papyrusRunning{ 0 };
		std::uint32_t papyrusMaxStacks{ 0 };
		char          papyrusTopScript[64]{};

		// Memory, in megabytes. 0 = not known.
		std::uint64_t gameMb{ 0 };
		std::uint64_t freeMb{ 0 };      // memory that the game can still get
		std::uint64_t vramUsedMb{ 0 };
		std::uint64_t vramBudgetMb{ 0 };

		// Engine values. 0 = not known.
		std::uint32_t drawCalls{ 0 };        // draw calls in each frame
		std::uint32_t references{ 0 };       // object references in use
		std::uint32_t referenceLimit{ 0 };   // the game can have this number of references at most
		std::uint32_t fullPlugins{ 0 };      // limit kFullPluginLimit
		std::uint32_t lightPlugins{ 0 };     // limit kLightPluginLimit
		// The "near the limit" plugin warning shows only from the game start
		// until 1 minute after the first loading screen. The "over the
		// limit" warning shows always: it can break save files.
		bool          pluginWarningTime{ true };

		// Memory trend with no loading screen in the period.
		bool  hasTrend{ false };
		float trendMbPerMinute{ 0 };        // over the whole period
		float trendRecentMbPerMinute{ 0 };  // over the last 2 minutes
		float trendMinutes{ 0 };
	};

	constexpr std::uint32_t kFullPluginLimit = 254;
	constexpr std::uint32_t kLightPluginLimit = 4096;

	enum class Severity : std::uint32_t
	{
		kNote = 1,
		kWarning = 2,
		kCritical = 3,
	};

	struct Warning
	{
		Severity severity{ Severity::kNote };
		char     id[24]{};     // stable name of the problem type, for example "fps-cpu"
		char     text[260]{};
	};

	// What limits the frame rate, from the frame data and the profiler.
	// The processor is the limit if a thread on the frame path has almost no
	// wait, or if a mod DLL keeps a large part of a processor core busy. If
	// all threads wait and the GPU keeps up, a frame cap or VSync holds the
	// rate.
	frames::Limit EffectiveLimit(const Inputs& a_in);

	// Writes up to a_max warnings, most severe first. Returns the count.
	int Evaluate(const Inputs& a_in, Warning* a_out, int a_max);
}
