#include "Advisor.h"

#include "../MemText.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace skydiag::advisor
{
	namespace
	{
		struct List
		{
			Warning* items;
			int      max;
			int      count;
		};

		void Add(List& a_list, Severity a_severity, const char* a_id, const char* a_fmt, ...)
		{
			if (a_list.count >= a_list.max) {
				return;
			}
			Warning& w = a_list.items[a_list.count++];
			w.severity = a_severity;
			std::snprintf(w.id, sizeof(w.id), "%s", a_id);
			va_list args;
			va_start(args, a_fmt);
			std::vsnprintf(w.text, sizeof(w.text), a_fmt, args);
			va_end(args);
		}

		// Threads on the frame path: the thread that shows the frames and
		// the threads that run SKSE tasks. If the roles are not known, all
		// sampled threads count.
		bool OnFramePath(const Inputs& a_in, const ThreadProfile& a_thread)
		{
			bool anyRole = false;
			for (int i = 0; i < a_in.threadCount; ++i) {
				anyRole = anyRole || a_in.threads[i].isRender || a_in.threads[i].isTask;
			}
			return !anyRole || a_thread.isRender || a_thread.isTask;
		}

		// The frame path thread that waits least. nullptr if no data.
		const ThreadProfile* BusiestThread(const Inputs& a_in)
		{
			const ThreadProfile* best = nullptr;
			for (int i = 0; i < a_in.threadCount; ++i) {
				const ThreadProfile& t = a_in.threads[i];
				if (OnFramePath(a_in, t) && (!best || t.waitShare < best->waitShare)) {
					best = &t;
				}
			}
			return best;
		}

		const char* ThreadName(const ThreadProfile& a_thread)
		{
			return a_thread.isRender ? "thread that shows the frames" : (a_thread.isTask ? "game update thread" : "busiest game thread");
		}

		// The mod DLL with the most own code, and the one with the most
		// waiting. nullptr if there is none.
		const ModLoad* TopMod(const Inputs& a_in, float ModLoad::* a_field)
		{
			const ModLoad* best = nullptr;
			for (int i = 0; i < a_in.modCount; ++i) {
				if (!best || a_in.mods[i].*a_field > best->*a_field) {
					best = &a_in.mods[i];
				}
			}
			return best;
		}

		bool CpuBound(const Inputs& a_in)
		{
			const ThreadProfile* busiest = BusiestThread(a_in);
			if (busiest && busiest->waitShare < 0.25f) {
				return true;
			}
			// A mod DLL that keeps half a processor core busy with its own
			// code slows the game also when its work moves between threads.
			const ModLoad* mod = TopMod(a_in, &ModLoad::ownCores);
			return mod && mod->ownCores >= 0.5f;
		}

		// Text for the most probable cause of processor-limited frames.
		void CpuCause(const Inputs& a_in, char* a_out, std::size_t a_cap)
		{
			const ModLoad*       own = TopMod(a_in, &ModLoad::ownCores);
			const ThreadProfile* t = BusiestThread(a_in);

			// The mod with the most work that is not waiting.
			const ModLoad* calls = nullptr;
			for (int i = 0; i < a_in.modCount; ++i) {
				const float work = a_in.mods[i].totalCores - a_in.mods[i].waitingCores;
				if (!calls || work > calls->totalCores - calls->waitingCores) {
					calls = &a_in.mods[i];
				}
			}

			if (own && own->ownCores >= 0.15f) {
				std::snprintf(a_out, a_cap, "%s uses %.0f%% of one processor core in its own code (about %.1f ms in each frame)", own->name,
					own->ownCores * 100.0f, static_cast<double>(own->ownCores * a_in.frames.averageMs));
			} else if (calls && (calls->totalCores - calls->waitingCores) >= 0.35f) {
				std::snprintf(a_out, a_cap, "%s is on the call stack for %.0f%% of one processor core (its own code and code that it calls or hooks)",
					calls->name, (calls->totalCores - calls->waitingCores) * 100.0f);
			} else if (a_in.hasPapyrus && a_in.papyrusRunning >= 8.0f) {
				std::snprintf(a_out, a_cap, "Papyrus has %.0f running script stacks (most: %s)", static_cast<double>(a_in.papyrusRunning),
					a_in.papyrusTopScript[0] ? a_in.papyrusTopScript : "not known");
			} else if (t && t->topModule[0] != '\0' && t->topModuleShare >= 0.30f) {
				if (t->topModuleIsGame) {
					std::snprintf(a_out, a_cap, "the game's own code uses %.0f%% of the %s (actors, physics, objects in view)",
						t->topModuleShare * 100.0f, ThreadName(*t));
				} else {
					if (a_in.drawCalls > 0) {
						std::snprintf(a_out, a_cap, "%s uses %.0f%% of the %s. The game makes %u draw calls in each frame: too many objects, lights or shadows in view",
							t->topModule, t->topModuleShare * 100.0f, ThreadName(*t), a_in.drawCalls);
					} else {
						std::snprintf(a_out, a_cap, "%s uses %.0f%% of the %s (for the graphics layer: too many draw calls, lights or shadows)",
							t->topModule, t->topModuleShare * 100.0f, ThreadName(*t));
					}
				}
			} else {
				std::snprintf(a_out, a_cap, "no one module stands out in the game threads");
			}
		}
	}

	frames::Limit EffectiveLimit(const Inputs& a_in)
	{
		if (a_in.threadCount == 0) {
			return a_in.frames.limit;
		}
		const bool cpuBound = CpuBound(a_in);
		if (a_in.frames.limit == frames::Limit::kCpu) {
			return cpuBound ? frames::Limit::kCpu : frames::Limit::kLimiter;
		}
		// A regular frame time looks like a cap, but if the game does not
		// wait in Present and the processor is the limit, it is not a cap.
		if (a_in.frames.limit == frames::Limit::kLimiter && a_in.frames.presentShare < 0.25f && cpuBound) {
			return frames::Limit::kCpu;
		}
		return a_in.frames.limit;
	}

	int Evaluate(const Inputs& a_in, Warning* a_out, int a_max)
	{
		List        list{ a_out, a_max, 0 };
		const auto& f = a_in.frames;

		// Memory that is almost full: the game can stop with no warning.
		if (a_in.freeMb != 0 && a_in.freeMb < 1024) {
			Add(list, Severity::kCritical, "memory-full", "Memory is almost full: the game uses %s and can get only %s more. A crash is probable.",
				MemText(a_in.gameMb).text, MemText(a_in.freeMb).text);
		} else if (a_in.freeMb != 0 && a_in.freeMb < 3072) {
			Add(list, Severity::kWarning, "memory-low", "Memory is low: the game uses %s and can get %s more.", MemText(a_in.gameMb).text,
				MemText(a_in.freeMb).text);
		}

		// Video memory.
		if (a_in.vramBudgetMb != 0 && a_in.vramUsedMb >= a_in.vramBudgetMb) {
			Add(list, Severity::kCritical, "vram-full",
				"Video memory is full (%s of %s). The system now moves textures to normal memory: expect stutter and low FPS.",
				MemText(a_in.vramUsedMb).text, MemText(a_in.vramBudgetMb).text);
		} else if (a_in.vramBudgetMb != 0 && a_in.vramUsedMb * 100 >= a_in.vramBudgetMb * 90) {
			Add(list, Severity::kWarning, "vram-high", "Video memory is almost full (%s of %s).", MemText(a_in.vramUsedMb).text,
				MemText(a_in.vramBudgetMb).text);
		}

		// Memory that grows with no loading screen. The growth must be there
		// over the whole period and also in the last 2 minutes. Memory that
		// grew before but is stable now gives no warning.
		if (a_in.hasTrend && a_in.trendMinutes >= 5.0f && a_in.trendMbPerMinute >= 30.0f && a_in.trendRecentMbPerMinute >= 30.0f) {
			if (a_in.freeMb != 0) {
				Add(list, Severity::kWarning, "memory-growth",
					"Game memory grows with no loading screen for %.0f minutes, now %s each minute: possible memory leak. At this rate memory is full in about %.0f minutes.",
					static_cast<double>(a_in.trendMinutes), MemText(static_cast<std::uint64_t>(a_in.trendRecentMbPerMinute)).text,
					static_cast<double>(a_in.freeMb) / static_cast<double>(a_in.trendRecentMbPerMinute));
			} else {
				Add(list, Severity::kWarning, "memory-growth", "Game memory grows with no loading screen for %.0f minutes, now %s each minute: possible memory leak.",
					static_cast<double>(a_in.trendMinutes), MemText(static_cast<std::uint64_t>(a_in.trendRecentMbPerMinute)).text);
			}
		}

		// Object references: the game has a fixed number of handles. When
		// they are all in use, the game cannot make a new reference.
		if (a_in.referenceLimit != 0 && a_in.references * 100ull >= a_in.referenceLimit * 95ull) {
			Add(list, Severity::kCritical, "references", "The game has almost no free object references (%u of %u in use). A crash is probable.",
				a_in.references, a_in.referenceLimit);
		} else if (a_in.referenceLimit != 0 && a_in.references * 100ull >= a_in.referenceLimit * 80ull) {
			Add(list, Severity::kWarning, "references", "The game uses %u of %u object references (%.0f%%). The game can crash when all are in use.",
				a_in.references, a_in.referenceLimit, static_cast<double>(a_in.references) * 100.0 / static_cast<double>(a_in.referenceLimit));
		}

		// Plugin counts. A count above the maximum is critical and stays:
		// it can break save files.
		if (a_in.fullPlugins > kFullPluginLimit || a_in.lightPlugins > kLightPluginLimit) {
			Add(list, Severity::kCritical, "plugins", "The plugin count is over the limit: %u of %u full plugins, %u of %u light plugins. This can break save files.",
				a_in.fullPlugins, kFullPluginLimit, a_in.lightPlugins, kLightPluginLimit);
		} else if (a_in.pluginWarningTime && (a_in.fullPlugins * 10 >= kFullPluginLimit * 9 || a_in.lightPlugins * 10 >= kLightPluginLimit * 9)) {
			Add(list, Severity::kWarning, "plugins", "The plugin count is near the limit: %u of %u full plugins, %u of %u light plugins.",
				a_in.fullPlugins, kFullPluginLimit, a_in.lightPlugins, kLightPluginLimit);
		}

		// Papyrus overload.
		if (a_in.settled && a_in.hasPapyrus && a_in.papyrusMaxStacks >= 500) {
			Add(list, Severity::kWarning, "papyrus-stacks", "Papyrus has %u script stacks (most: %s). Scripts are slow and stack dumps are possible.",
				a_in.papyrusMaxStacks, a_in.papyrusTopScript[0] ? a_in.papyrusTopScript : "not known");
		}

		// Frame rate below 60.
		if (a_in.settled && f.frames >= 20 && f.fps < a_in.fpsBelow) {
			switch (EffectiveLimit(a_in)) {
			case frames::Limit::kGpu:
				Add(list, Severity::kWarning, "fps-gpu", "%.0f FPS. Cause: the GPU. It is %.1f frames behind the game.", static_cast<double>(f.fps),
					static_cast<double>(f.gpuQueue));
				break;
			case frames::Limit::kLimiter:
				{
					const ModLoad*       waiter = TopMod(a_in, &ModLoad::waitingCores);
					const ThreadProfile* busiest = BusiestThread(a_in);
					if (waiter && waiter->waitingCores >= 0.20f) {
						Add(list, Severity::kNote, "fps-cap", "%.0f FPS. Cause: a frame cap or VSync. The game waits %.0f%% of the time inside calls from %s.",
							static_cast<double>(f.fps), waiter->waitingCores * 100.0f, waiter->name);
					} else if (busiest && busiest->waitShare >= 0.25f) {
						Add(list, Severity::kNote, "fps-cap", "%.0f FPS. Cause: a frame cap or VSync. The game threads wait %.0f%% of their time or more.",
							static_cast<double>(f.fps), busiest->waitShare * 100.0f);
					} else {
						Add(list, Severity::kNote, "fps-cap", "%.0f FPS. Cause: VSync or a frame cap holds the game at this rate.", static_cast<double>(f.fps));
					}
				}
				break;
			case frames::Limit::kCpu:
				{
					char cause[220];
					CpuCause(a_in, cause, sizeof(cause));
					Add(list, Severity::kWarning, "fps-cpu", "%.0f FPS. Cause: the CPU. Most probable: %s.", static_cast<double>(f.fps), cause);
				}
				break;
			default:
				break;
			}
		}

		// Stutter: some frames are much slower than the average.
		if (a_in.settled && f.frames >= 60 && f.worstMs >= 50.0f && f.worstMs >= f.averageMs * 3.0f) {
			if (a_in.stutterCause[0] != '\0') {
				Add(list, Severity::kNote, "stutter", "Stutter: the slowest frame of the last seconds took %.0f ms (average %.1f ms). In the last slow frame: %s.",
					static_cast<double>(f.worstMs), static_cast<double>(f.averageMs), a_in.stutterCause);
			} else {
				Add(list, Severity::kNote, "stutter", "Stutter: the slowest frame of the last seconds took %.0f ms (average %.1f ms).",
					static_cast<double>(f.worstMs), static_cast<double>(f.averageMs));
			}
		}

		// Input latency.
		if (a_in.settled && f.frames >= 20 && f.latencyMs >= 60.0f) {
			if (f.gpuQueue >= 0.75f) {
				Add(list, Severity::kWarning, "latency", "Input latency about %.0f ms. Cause: the GPU has %.1f frames in its queue, and each one adds a frame time.",
					static_cast<double>(f.latencyMs), static_cast<double>(f.gpuQueue));
			} else {
				Add(list, Severity::kWarning, "latency", "Input latency about %.0f ms. Cause: the frame time (%.0f ms).", static_cast<double>(f.latencyMs),
					static_cast<double>(f.averageMs));
			}
		}

		std::stable_sort(a_out, a_out + list.count, [](const Warning& a_lhs, const Warning& a_rhs) {
			return static_cast<std::uint32_t>(a_lhs.severity) > static_cast<std::uint32_t>(a_rhs.severity);
		});
		return list.count;
	}
}
