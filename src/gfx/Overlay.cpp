#include "Overlay.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <iterator>

#include "../Abyssus/API.h"
#include "../crash/MemTrack.h"
#include "../crash/Profiler.h"
#include "../crash/Session.h"
#include "../MemText.h"
#include "../OverlayLayout.h"
#include "../Report.h"
#include "../Papyrus.h"
#include "Advisor.h"
#include "FrameStats.h"

// The windows of the overlay go through AbyssusLib (Abyssus/API.h): this file
// makes their content, AbyssusLib draws it. This file has no Dear ImGui.

namespace skydiag::overlay
{
	namespace
	{
		constexpr const char* kSource = "SkyrimDiagnostics";
		using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

		constexpr int kPresentSlot = 8;  // position of Present in the IDXGISwapChain function table
		constexpr int kMaxQueries = 8;
		Options g_options;

		std::atomic<PresentFn> g_originalPresent{ nullptr };
		char                   g_version[64] = "";

		// All data below is used on the render thread only.
		ID3D11Device*        g_device = nullptr;
		ID3D11DeviceContext* g_context = nullptr;
		IDXGIAdapter3*       g_adapter = nullptr;
		bool                 g_ready = false;
		bool                 g_failed = false;
		// F11 in AbyssusLib shows or hides the windows (Diag in its configurations).

		LARGE_INTEGER g_frequency{};
		LARGE_INTEGER g_lastPresentStart{};

		// One event query for each submitted frame. A query is complete when
		// the GPU has done all the work of that frame.
		ID3D11Query* g_queries[kMaxQueries] = {};
		bool         g_queryPending[kMaxQueries] = {};

		std::atomic<std::uint64_t> g_vramUsed{ 0 };
		std::atomic<std::uint64_t> g_vramBudget{ 0 };

		// Draw calls. The four draw functions of the device context are
		// hooked and count each call.
		using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
		using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
		using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
		using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);

		DrawIndexedFn          g_originalDrawIndexed = nullptr;
		DrawFn                 g_originalDraw = nullptr;
		DrawIndexedInstancedFn g_originalDrawIndexedInstanced = nullptr;
		DrawInstancedFn        g_originalDrawInstanced = nullptr;

		// Since the last Present. Not atomic on purpose: the game makes
		// thousands of draw calls in each frame, and an atomic add for each
		// one costs time. A lost count from two threads at the same time is
		// not important.
		std::uint32_t              g_drawCalls = 0;
		std::atomic<std::uint32_t> g_drawCallsPerFrame{ 0 };  // average
		std::uint64_t              g_drawCallSum = 0;
		std::uint32_t              g_drawCallFrames = 0;
		LARGE_INTEGER              g_drawCallWindow{};

		void STDMETHODCALLTYPE HookedDrawIndexed(ID3D11DeviceContext* a_context, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex)
		{
			++g_drawCalls;
			g_originalDrawIndexed(a_context, a_indexCount, a_startIndex, a_baseVertex);
		}

		void STDMETHODCALLTYPE HookedDraw(ID3D11DeviceContext* a_context, UINT a_vertexCount, UINT a_startVertex)
		{
			++g_drawCalls;
			g_originalDraw(a_context, a_vertexCount, a_startVertex);
		}

		void STDMETHODCALLTYPE HookedDrawIndexedInstanced(ID3D11DeviceContext* a_context, UINT a_indexCount, UINT a_instanceCount, UINT a_startIndex,
			INT a_baseVertex, UINT a_startInstance)
		{
			++g_drawCalls;
			g_originalDrawIndexedInstanced(a_context, a_indexCount, a_instanceCount, a_startIndex, a_baseVertex, a_startInstance);
		}

		void STDMETHODCALLTYPE HookedDrawInstanced(ID3D11DeviceContext* a_context, UINT a_vertexCount, UINT a_instanceCount, UINT a_startVertex,
			UINT a_startInstance)
		{
			++g_drawCalls;
			g_originalDrawInstanced(a_context, a_vertexCount, a_instanceCount, a_startVertex, a_startInstance);
		}

		// Puts a_hook in a slot of a function table and gives the function
		// that was there.
		template <class F>
		bool PatchSlot(void** a_table, int a_slot, F a_hook, F& a_original)
		{
			void** slot = &a_table[a_slot];
			DWORD  oldProtect = 0;
			if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				return false;
			}
			a_original = reinterpret_cast<F>(*slot);
			InterlockedExchangePointer(slot, reinterpret_cast<void*>(a_hook));
			VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
			return true;
		}

		void HookDrawCalls()
		{
			// Positions of the draw functions in the ID3D11DeviceContext
			// function table.
			void** table = *reinterpret_cast<void***>(g_context);
			const bool ok = PatchSlot(table, 12, &HookedDrawIndexed, g_originalDrawIndexed) && PatchSlot(table, 13, &HookedDraw, g_originalDraw) &&
			                PatchSlot(table, 20, &HookedDrawIndexedInstanced, g_originalDrawIndexedInstanced) &&
			                PatchSlot(table, 21, &HookedDrawInstanced, g_originalDrawInstanced);
			session::Event(ok ? "Draw call counter: on" : "Draw call counter: could not start");
		}

		std::atomic<unsigned> g_references{ 0 };
		std::atomic<unsigned> g_referenceLimit{ 0 };
		std::atomic<unsigned> g_fullPlugins{ 0 };
		std::atomic<unsigned> g_lightPlugins{ 0 };
		std::atomic<unsigned> g_gameFullPlugins{ 0 };
		std::atomic<unsigned> g_gameLightPlugins{ 0 };
		std::atomic<unsigned> g_animations{ 0 };
		std::atomic<bool>     g_animationsKnown{ false };
		std::atomic<LONGLONG> g_inputTime{ 0 };  // counter value of the first input read since the last Present
		std::atomic<bool>     g_pluginWarningTime{ true };
		// True from the game start until the main menu closes, and again
		// when it opens. Only the plugin warning is used in this time.
		std::atomic<bool>     g_inMainMenu{ true };
		bool                  g_settled = false;    // render thread only

		LARGE_INTEGER g_lastVramCheck{};
		std::uint64_t g_lastThreadCpu100ns = 0;
		LARGE_INTEGER g_lastLongFrameEvent{};

		std::uint64_t MicrosBetween(const LARGE_INTEGER& a_from, const LARGE_INTEGER& a_to)
		{
			return static_cast<std::uint64_t>((a_to.QuadPart - a_from.QuadPart) * 1000000 / g_frequency.QuadPart);
		}

		// Processor time (kernel and user) of the calling thread.
		std::uint64_t ThreadCpu100ns()
		{
			FILETIME created, exited, kernel, user;
			if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
				return 0;
			}
			const auto toInt = [](const FILETIME& a_time) {
				return (static_cast<std::uint64_t>(a_time.dwHighDateTime) << 32) | a_time.dwLowDateTime;
			};
			return toInt(kernel) + toInt(user);
		}

		bool Setup(IDXGISwapChain* a_swapChain)
		{
			if (FAILED(a_swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))) || !g_device) {
				return false;
			}
			g_device->GetImmediateContext(&g_context);

			// Video memory data needs IDXGIAdapter3. It is optional.
			IDXGIDevice* dxgiDevice = nullptr;
			if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) && dxgiDevice) {
				IDXGIAdapter* adapter = nullptr;
				if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter) {
					adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&g_adapter));
					adapter->Release();
				}
				dxgiDevice->Release();
			}

			D3D11_QUERY_DESC queryDesc{};
			queryDesc.Query = D3D11_QUERY_EVENT;
			for (auto& query : g_queries) {
				g_device->CreateQuery(&queryDesc, &query);
			}

			QueryPerformanceFrequency(&g_frequency);
			HookDrawCalls();
			return true;
		}

		// Counts the older frames that the GPU has not completed, then adds
		// a query for the frame that is submitted now.
		std::uint32_t UpdateGpuQueue()
		{
			std::uint32_t pending = 0;
			int           freeSlot = -1;
			for (int i = 0; i < kMaxQueries; ++i) {
				if (!g_queries[i]) {
					continue;
				}
				if (g_queryPending[i]) {
					BOOL done = FALSE;
					if (g_context->GetData(g_queries[i], &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && done) {
						g_queryPending[i] = false;
					} else {
						++pending;
					}
				}
				if (!g_queryPending[i] && freeSlot < 0) {
					freeSlot = i;
				}
			}
			if (freeSlot >= 0) {
				g_context->End(g_queries[freeSlot]);
				g_queryPending[freeSlot] = true;
			}
			return pending;
		}

		void UpdateVideoMemory(const LARGE_INTEGER& a_now)
		{
			if (!g_adapter || MicrosBetween(g_lastVramCheck, a_now) < 1000000) {
				return;
			}
			g_lastVramCheck = a_now;
			DXGI_QUERY_VIDEO_MEMORY_INFO info{};
			if (SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
				g_vramUsed = info.CurrentUsage;
				g_vramBudget = info.Budget;
			}
		}

		// A warning stays on the screen for this time after its condition
		// ended, so that the player can read it.

		struct ShownWarning
		{
			advisor::Warning warning;
			LARGE_INTEGER    lastActive;
			bool             active;
			bool             used;
		};
		ShownWarning  g_shown[10] = {};
		int           g_warningCount = 0;  // warnings to draw
		LARGE_INTEGER g_lastAdvice{};
		frames::Limit g_effectiveLimit = frames::Limit::kUnknown;
		char          g_stutterCause[120] = "";
		LARGE_INTEGER g_stutterTime{};

		abyssus::Level NoteLevel(advisor::Severity a_severity)
		{
			return a_severity == advisor::Severity::kCritical ? abyssus::Level::kError :
			       a_severity == advisor::Severity::kWarning  ? abyssus::Level::kWarning :
			                                                    abyssus::Level::kInfo;
		}

		// Warnings that are used in the main menu: the plugin count, and a
		// crash that is probable (memory or video memory full).
		bool MainMenuWarning(const char* a_id)
		{
			return std::strcmp(a_id, "plugins") == 0 || std::strcmp(a_id, "memory-full") == 0 || std::strcmp(a_id, "vram-full") == 0;
		}

		// The plugin warning is a note without time limit in the queue: it
		// stays while the warning shows. Over the limit: always. Near the
		// limit: until 1 minute after the first cell is loaded, plus the
		// warning hold time.
		std::string g_pluginNote;  // the text that is in the queue, empty = none

		void UpdatePluginNote()
		{
			const ShownWarning* plugins = nullptr;
			for (const auto& shown : g_shown) {
				if (shown.used && std::strcmp(shown.warning.id, "plugins") == 0) {
					plugins = &shown;
				}
			}
			const std::string text = plugins ? plugins->warning.text : "";
			if (text != g_pluginNote) {
				g_pluginNote = text;
				abyssus::SetNote(kSource, "plugins", text.c_str(), plugins ? NoteLevel(plugins->warning.severity) : abyssus::Level::kInfo);
			}
		}

		// Collects the current measurements and makes the warning list.
		// Runs two times each second.
		void UpdateWarnings(const LARGE_INTEGER& a_now)
		{
			if (MicrosBetween(g_lastAdvice, a_now) < 500000) {
				return;
			}
			g_lastAdvice = a_now;

			static advisor::Inputs in;
			in = {};
			in.frames = frames::Latest();
			in.fpsBelow = g_options.fpsBelow;

			static profiler::ThreadInfo threads[16];
			static profiler::Snapshot   profile;
			const int                   threadCount = profiler::ListThreads(threads, static_cast<int>(std::size(threads)), 5000000);
			for (int i = 0; i < threadCount && in.threadCount < static_cast<int>(std::size(in.threads)); ++i) {
				if (threads[i].samples < 100) {
					continue;  // sampled for a short time only
				}
				auto& out = in.threads[in.threadCount++];
				out.isRender = threads[i].isRender;
				out.isTask = threads[i].isTask;
				out.waitShare = threads[i].waitShare;
				profiler::GetSnapshot(profile, 5000000, threads[i].id);
				if (profile.count > 0) {
					strncpy_s(out.topModule, profile.entries[0].name, _TRUNCATE);
					out.topModuleShare = profile.entries[0].share;
					out.topModuleIsGame = _strnicmp(profile.entries[0].name, "Skyrim", 6) == 0;
				}
			}
			static profiler::ModTotal mods[4];
			in.modCount = profiler::GetMods(mods, static_cast<int>(std::size(mods)), 5000000);
			for (int i = 0; i < in.modCount; ++i) {
				strncpy_s(in.mods[i].name, mods[i].name, _TRUNCATE);
				in.mods[i].ownCores = mods[i].ownCores;
				in.mods[i].waitingCores = mods[i].waitingCores;
				in.mods[i].totalCores = mods[i].totalCores;
			}

			static papyrus::Snapshot scripts;
			papyrus::GetSnapshot(scripts);
			if (scripts.valid) {
				in.hasPapyrus = true;
				in.papyrusRunning = scripts.runningStacks;
				in.papyrusMaxStacks = scripts.maxStacks;
				if (scripts.count > 0) {
					strncpy_s(in.papyrusTopScript, scripts.entries[0].name, _TRUNCATE);
				}
			}

			session::Latest latest;
			if (session::GetLatest(latest)) {
				in.gameMb = latest.commit >> 20;
				in.freeMb = latest.systemCommitFree >> 20;
			}
			in.vramUsedMb = g_vramUsed.load() >> 20;
			in.vramBudgetMb = g_vramBudget.load() >> 20;
			in.drawCalls = g_drawCallsPerFrame.load();
			in.references = g_references.load();
			in.referenceLimit = g_referenceLimit.load();
			in.fullPlugins = g_fullPlugins.load();
			in.lightPlugins = g_lightPlugins.load();
			in.pluginWarningTime = g_pluginWarningTime.load();
			in.hasTrend = session::GetMemoryTrend(in.trendMbPerMinute, in.trendMinutes, in.trendRecentMbPerMinute);

			// No frame, stutter or Papyrus warnings (and no value colors) in
			// the main menu, during a load and for 10 s after it.
			in.settled = !g_inMainMenu.load() && session::SecondsSinceLoading() >= 10.0f;
			g_settled = in.settled;
			if (g_stutterCause[0] != '\0' && MicrosBetween(g_stutterTime, a_now) < 30'000'000) {
				strncpy_s(in.stutterCause, g_stutterCause, _TRUNCATE);
			}
			g_effectiveLimit = advisor::EffectiveLimit(in);

			static advisor::Warning current[8];
			int                     count = advisor::Evaluate(in, current, static_cast<int>(std::size(current)));

			// Main menu: the frame, memory and script values of the main menu
			// and of the game start are not useful. Only the plugin count and
			// a probable crash (memory or video memory full) are. Other
			// warnings that show are removed.
			if (g_inMainMenu.load()) {
				int kept = 0;
				for (int i = 0; i < count; ++i) {
					if (MainMenuWarning(current[i].id)) {
						current[kept++] = current[i];
					}
				}
				count = kept;
				for (auto& shown : g_shown) {
					if (shown.used && !MainMenuWarning(shown.warning.id)) {
						shown.used = false;
					}
				}
			}

			for (auto& shown : g_shown) {
				shown.active = false;
			}
			for (int i = 0; i < count; ++i) {
				ShownWarning* slot = nullptr;
				for (auto& shown : g_shown) {
					if (shown.used && std::strcmp(shown.warning.id, current[i].id) == 0) {
						slot = &shown;
						break;
					}
				}
				if (!slot) {
					// A warning that is new becomes an event, so reports show it.
					// The first word is the level. The session report uses
					// it for the color.
					const char* level = current[i].severity == advisor::Severity::kCritical ? "Critical" :
					                    current[i].severity == advisor::Severity::kWarning ? "Warning" : "Note";
					session::Event("%s: %s", level, current[i].text);
					// And one time a note in the notification queue of AbyssusLib
					// (the plugin warning: see UpdatePluginNote).
					if (std::strcmp(current[i].id, "plugins") != 0) {
						abyssus::Notify(kSource, current[i].text, 10.0f, NoteLevel(current[i].severity));
					}
					// The session report lists the suggestions for this problem type.
					report::NoteProblem(current[i].id);
					for (auto& shown : g_shown) {
						if (!shown.used) {
							slot = &shown;
							break;
						}
					}
				}
				if (slot) {
					slot->warning = current[i];
					slot->lastActive = a_now;
					slot->active = true;
					slot->used = true;
				}
			}
			g_warningCount = 0;
			for (auto& shown : g_shown) {
				if (shown.used && !shown.active &&
					MicrosBetween(shown.lastActive, a_now) >= static_cast<std::uint64_t>(g_options.warningHoldSeconds * 1000000.0f)) {
					shown.used = false;
				}
				if (shown.used) {
					++g_warningCount;
				}
			}
			UpdatePluginNote();
		}

		// The two windows, and the builder that Part, Line and EndLine write to.
		abyssus::WindowBuilder  g_dataWindow;
		abyssus::WindowBuilder  g_warningsWindow;
		abyssus::WindowBuilder* g_out = &g_dataWindow;
		bool                    g_warningsShown = false;  // the warnings window is sent to AbyssusLib

		constexpr AbyssusColor kRed{ 1.0f, 0.35f, 0.3f, 1.0f };
		constexpr AbyssusColor kYellow{ 1.0f, 0.8f, 0.25f, 1.0f };
		constexpr AbyssusColor kGrey{ 0.8f, 0.85f, 0.9f, 1.0f };

		// Colors of the values: 0 = normal, 1 = yellow (near a limit),
		// 2 = red (at or over a limit).
		AbyssusColor LevelColor(int a_level)
		{
			return a_level >= 2 ? kRed : a_level == 1 ? kYellow : abyssus::WindowBuilder::kDefault;
		}

		void ClearWarningsWindow()
		{
			if (g_warningsShown) {
				abyssus::ClearWindow(kSource, "warnings");
				g_warningsShown = false;
			}
		}

		// The warnings window, under the data window.
		void BuildWarnings()
		{
			if (g_warningCount == 0) {
				ClearWarningsWindow();
				return;
			}
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			g_warningsWindow.Clear();
			// Most severe first; a warning that ended is dim and shows
			// how long ago it ended.
			for (int level = 3; level >= 1; --level) {
				for (const auto& shown : g_shown) {
					if (!shown.used || static_cast<int>(shown.warning.severity) != level) {
						continue;
					}
					AbyssusColor color = level == 3 ? kRed : level == 2 ? kYellow : kGrey;
					if (shown.active) {
						g_warningsWindow.Text(color, "%s", shown.warning.text);
					} else {
						color.a = 0.6f;
						g_warningsWindow.Text(color, "%s (ended %llu s ago)", shown.warning.text,
							static_cast<unsigned long long>(MicrosBetween(shown.lastActive, now) / 1000000));
					}
					g_warningsWindow.NewLine();
				}
			}
			const AbyssusWindow window = g_warningsWindow.Make(0.7f, 560.0f, "data");
			abyssus::SetWindow(kSource, "warnings", &window);
			g_warningsShown = true;
		}

		// Level for a value that is bad when it is high, and for one that
		// is bad when it is low.
		int High(double a_value, double a_yellow, double a_red)
		{
			return a_value >= a_red ? 2 : (a_value >= a_yellow ? 1 : 0);
		}

		int Low(double a_value, double a_yellow, double a_red)
		{
			return a_value < a_red ? 2 : (a_value < a_yellow ? 1 : 0);
		}

		// One part of a line, in the color of its level. The next part
		// goes on the same line. EndLine ends the line.
		void Part(int a_level, const char* a_fmt, ...)
		{
			char    text[240];
			va_list args;
			va_start(args, a_fmt);
			std::vsnprintf(text, sizeof(text), a_fmt, args);
			va_end(args);
			g_out->Text(LevelColor(a_level), "%s", text);
		}

		void EndLine()
		{
			g_out->NewLine();
		}

		// A full line in the normal color.
		void Line(const char* a_fmt, ...)
		{
			char    text[240];
			va_list args;
			va_start(args, a_fmt);
			std::vsnprintf(text, sizeof(text), a_fmt, args);
			va_end(args);
			g_out->Text(abyssus::WindowBuilder::kDefault, "%s", text);
			g_out->NewLine();
		}

		double Percent(double a_value, double a_maximum)
		{
			return a_maximum > 0.0 ? a_value * 100.0 / a_maximum : 0.0;
		}

		// The layout of the overlay, from the configuration file.
		std::vector<layout::Part> g_layout;

		void BuildWindow()
		{
			const frames::Summary s = frames::Latest();

			g_dataWindow.Clear();
			g_out = &g_dataWindow;
			{
				Line("SkyrimDiagnostics %s (F11: AbyssusLib configurations)", g_version);
				g_out->Separator();

				// The colors of the frame values are for the settled game
				// only: a loading screen has slow frames and that is normal.
				const bool   live = g_settled && s.frames >= 20;
				const double target = g_options.fpsBelow;
				const double targetMs = target > 0.0 ? 1000.0 / target : 17.5;

				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);

				// Data that more than one part uses. It is read one time for
				// the window, and only if a part in the layout uses it.
				session::Latest latest;
				bool            haveLatest = false;
				bool            latestRead = false;
				const auto      readLatest = [&]() {
					     if (!latestRead) {
						     latestRead = true;
						     haveLatest = session::GetLatest(latest);
					     }
					     return haveLatest;
				};

				static profiler::ThreadInfo shownThreads[16];
				static profiler::Snapshot   shownProfiles[16];
				static profiler::ModTotal   shownMods[4];
				static int                  shownThreadCount = 0;
				static int                  shownModCount = 0;
				static LARGE_INTEGER        lastProfile{};
				bool                        profileRead = false;
				const auto                  readProfile = [&]() {
					                 if (profileRead) {
						                 return;
					                 }
					                 profileRead = true;
					                 if (MicrosBetween(lastProfile, now) >= 500000) {
						                 lastProfile = now;
						                 shownThreadCount = profiler::ListThreads(shownThreads, static_cast<int>(std::size(shownThreads)), 5000000);
						                 for (int i = 0; i < shownThreadCount; ++i) {
							                 profiler::GetSnapshot(shownProfiles[i], 5000000, shownThreads[i].id);
						                 }
						                 shownModCount = profiler::GetMods(shownMods, static_cast<int>(std::size(shownMods)), 5000000);
					                 }
				};

				static papyrus::Snapshot scripts;
				bool                     scriptsRead = false;
				const auto               readScripts = [&]() {
					              if (!scriptsRead) {
						              scriptsRead = true;
						              papyrus::GetSnapshot(scripts);
					              }
					              return scripts.valid;
				};

				for (const layout::Part part : g_layout) {
					switch (part) {
					case layout::Part::kSeparator:
						g_out->Separator();
						break;
					case layout::Part::kFps:
						Part(0, "FPS ");
						Part(live ? Low(s.fps, target, target * 0.75) : 0, "%.0f", s.fps);
						Part(0, "   frame ");
						Part(live ? High(s.averageMs, targetMs, targetMs / 0.75) : 0, "%.1f ms", s.averageMs);
						Part(0, "   1%% low ");
						Part(live ? High(s.low1Ms, targetMs * 1.5, targetMs * 3.0) : 0, "%.1f ms", s.low1Ms);
						Part(0, "   worst ");
						Part(live ? High(s.worstMs, 50.0, 100.0) : 0, "%.1f ms", s.worstMs);
						EndLine();
						break;
					case layout::Part::kGraph:
						{
							static float history[300];
							const int    count = frames::History(history, 300);
							g_out->Graph(history, count, 0.0f, 50.0f, 380.0f, 50.0f);
						}
						break;
					case layout::Part::kPresent:
						Part(0, "Wait in Present %.1f ms (%.0f%%)   GPU queue ", s.presentMs, s.presentShare * 100.0f);
						Part(live ? High(s.gpuQueue, 0.75, 2.0) : 0, "%.1f frames", s.gpuQueue);
						EndLine();
						break;
					case layout::Part::kLimit:
						Line("Limit: %s", frames::LimitText(g_effectiveLimit));
						break;
					case layout::Part::kDrawCalls:
						Line("Draw calls %u each frame", g_drawCallsPerFrame.load());
						break;
					case layout::Part::kLatency:
						Part(0, "Input latency ");
						Part(live ? High(s.latencyMs, 60.0, 100.0) : 0, "%.0f ms", s.latencyMs);
						Part(0, s.latencyMeasured ? " (input read to GPU done, measured)" : " (input read to GPU done, estimate)");
						EndLine();
						break;
					case layout::Part::kResponse:
						if (readLatest()) {
							Part(0, "Update thread response ");
							Part(High(latest.updateResponseMs, 100.0, 1000.0), "%u ms", latest.updateResponseMs);
							EndLine();
						}
						break;
					case layout::Part::kMemory:
						if (readLatest()) {
							// The second value is the most that the game can have now:
							// what it uses plus what the system can still give.
							const std::uint64_t used = latest.commit >> 20;
							const std::uint64_t maximum = (latest.commit + latest.systemCommitFree) >> 20;
							const double        percent = Percent(static_cast<double>(used), static_cast<double>(maximum));
							Part(0, "Game memory ");
							Part(High(percent, 80.0, 90.0), "%s / %s (%.0f%%)", MemText(used).text, MemText(maximum).text, percent);
							EndLine();
						}
						break;
					case layout::Part::kVram:
						if (g_vramBudget.load() != 0) {
							const std::uint64_t used = g_vramUsed.load() >> 20;
							const std::uint64_t maximum = g_vramBudget.load() >> 20;
							const double        percent = Percent(static_cast<double>(used), static_cast<double>(maximum));
							Part(0, "Video memory ");
							Part(High(percent, 80.0, 90.0), "%s / %s (%.0f%%)", MemText(used).text, MemText(maximum).text, percent);
							EndLine();
						} else {
							Line("Video memory: not available");
						}
						break;
					case layout::Part::kReferences:
						if (const unsigned limit = g_referenceLimit.load(); limit != 0) {
							const unsigned used = g_references.load();
							const double   percent = Percent(used, limit);
							Part(0, "Object references ");
							Part(High(percent, 80.0, 95.0), "%u / %u (%.0f%%)", used, limit, percent);
							EndLine();
						}
						break;
					case layout::Part::kPlugins:
						if (g_referenceLimit.load() != 0) {
							// Plugins: game files / other files (mods) / maximum.
							const unsigned full = g_fullPlugins.load();
							const unsigned light = g_lightPlugins.load();
							const unsigned gameFull = (std::min)(g_gameFullPlugins.load(), full);
							const unsigned gameLight = (std::min)(g_gameLightPlugins.load(), light);
							const auto     level = [](unsigned a_count, unsigned a_limit) { return a_count > a_limit ? 2 : (a_count * 10 >= a_limit * 9 ? 1 : 0); };
							Part(0, "Plugins (game / mods / maximum): full ");
							Part(level(full, advisor::kFullPluginLimit), "%u / %u / %u (%.0f%%)", gameFull, full - gameFull, advisor::kFullPluginLimit,
								Percent(full, advisor::kFullPluginLimit));
							Part(0, "   light ");
							Part(level(light, advisor::kLightPluginLimit), "%u / %u / %u (%.0f%%)", gameLight, light - gameLight, advisor::kLightPluginLimit,
								Percent(light, advisor::kLightPluginLimit));
							EndLine();
						}
						break;
					case layout::Part::kAnimations:
						if (g_animationsKnown.load()) {
							Line("Loaded animations %u", g_animations.load());
						}
						break;
					case layout::Part::kModMemory:
						// Heap memory of the mod DLLs with the most memory in use.
						if (g_options.memoryByDll) {
							static memtrack::Entry memory[3];
							static int             memoryCount = 0;
							static LARGE_INTEGER   lastMemory{};
							if (MicrosBetween(lastMemory, now) >= 1000000) {
								lastMemory = now;
								memoryCount = memtrack::GetTop(memory, static_cast<int>(std::size(memory)));
							}
							bool header = false;
							for (int i = 0; i < memoryCount; ++i) {
								const std::int64_t megabytes = memory[i].inUseBytes / (1024 * 1024);
								if (megabytes < 1) {
									continue;
								}
								if (!header) {
									Line("Heap memory in use by mod DLL (largest)");
									header = true;
								}
								Line("  %-20s  %s", MemText(static_cast<std::uint64_t>(megabytes)).text, memory[i].name);
							}
						}
						break;
					case layout::Part::kThreads:
						readProfile();
						if (shownThreadCount > 0) {
							Line("Busy threads, last 5 s: work %% of the thread time, and where");
							int lines = 0;
							for (int i = 0; i < shownThreadCount && lines < 6; ++i) {
								const auto& thread = shownThreads[i];
								const float work = 1.0f - thread.waitShare;
								if (thread.samples < 100 || (work < 0.03f && !thread.isRender)) {
									continue;
								}
								char where[160] = "";
								Out  out{ where, sizeof(where), 0 };
								for (int e = 0; e < shownProfiles[i].count && e < 2; ++e) {
									if (shownProfiles[i].entries[e].share < 0.02f) {
										break;
									}
									Put(out, "%s%s %.0f%%", e == 0 ? "" : ", ", shownProfiles[i].entries[e].name, shownProfiles[i].entries[e].share * 100.0f);
								}
								// A frame or task thread with almost no wait limits the frame rate.
								const bool framePath = thread.isRender || thread.isTask;
								Part(0, "  %5lu %-7s ", static_cast<unsigned long>(thread.id), thread.isRender ? "frames" : (thread.isTask ? "tasks" : ""));
								Part(framePath && live ? High(work, 0.75, 0.90) : 0, "%3.0f%%", work * 100.0f);
								Part(0, "  %s", where);
								EndLine();
								++lines;
							}
						} else {
							char status[240];
							profiler::Status(status, sizeof(status));
							Line("Profiler: %s", g_options.profiler ? status : "off in the settings file");
						}
						break;
					case layout::Part::kModDlls:
						{
							readProfile();
							bool header = false;
							for (int i = 0; i < shownModCount; ++i) {
								if (shownMods[i].totalCores < 0.01f) {
									continue;
								}
								if (!header) {
									Line("Mod DLLs, %% of one core: own code / waiting / total");
									header = true;
								}
								Part(0, "  ");
								Part(High(shownMods[i].ownCores, 0.15, 0.50), "%4.1f", shownMods[i].ownCores * 100.0f);
								Part(0, " / %4.1f / %4.1f  %s", shownMods[i].waitingCores * 100.0f, shownMods[i].totalCores * 100.0f, shownMods[i].name);
								EndLine();
							}
						}
						break;
					case layout::Part::kPapyrus:
						if (readScripts()) {
							Part(0, "Papyrus stacks: %.1f running, %.1f waiting (maximum ", scripts.runningStacks, scripts.waitingStacks);
							Part(g_settled ? High(scripts.maxStacks, 250.0, 500.0) : 0, "%u", scripts.maxStacks);
							Part(0, ")");
							EndLine();
						} else {
							Line("Papyrus sampler: %s", papyrus::Status());
						}
						break;
					case layout::Part::kScripts:
						if (readScripts()) {
							for (int i = 0; i < scripts.count && i < 4; ++i) {
								Line("  %4.1f run  %5.1f wait  %s", scripts.entries[i].running, scripts.entries[i].waiting, scripts.entries[i].name);
							}
						}
						break;
					case layout::Part::kFunctions:
						if (readScripts()) {
							if (scripts.timedCount > 0) {
								// Measured: time in each script function.
								for (int i = 0; i < scripts.timedCount && i < 4; ++i) {
									if (scripts.timed[i].msPerSecond < 0.01f) {
										break;
									}
									if (i == 0) {
										Line("Papyrus functions: ms each second / calls each second");
									}
									Part(0, "  ");
									Part(High(scripts.timed[i].msPerSecond, 20.0, 100.0), "%6.2f", scripts.timed[i].msPerSecond);
									Part(0, " / %6.0f  %s", scripts.timed[i].callsPerSecond, scripts.timed[i].name);
									EndLine();
								}
							} else {
								for (int i = 0; i < scripts.functionCount && i < 3; ++i) {
									if (scripts.functions[i].running < 0.05f) {
										break;
									}
									if (i == 0) {
										Line("Papyrus functions that run most (from samples)");
									}
									Line("  %4.2f  %s", scripts.functions[i].running, scripts.functions[i].name);
								}
							}
						}
						break;
					}
				}
			}
			const AbyssusWindow window = g_dataWindow.Make(0.6f);
			abyssus::SetWindow(kSource, "data", &window);
		}

		HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags)
		{
			const PresentFn original = g_originalPresent.load();

			if ((a_flags & DXGI_PRESENT_TEST) != 0 || g_failed) {
				return original(a_swapChain, a_syncInterval, a_flags);
			}
			if (!g_ready) {
				if (!Setup(a_swapChain)) {
					g_failed = true;
					session::Event("Overlay: setup failed");
					return original(a_swapChain, a_syncInterval, a_flags);
				}
				g_ready = true;
				session::Event("Overlay: ready");
				// The thread that calls Present is the game thread.
				if (g_options.profiler) {
					profiler::Start(GetCurrentThreadId());
				}
				QueryPerformanceCounter(&g_lastPresentStart);
			}

			// The thread that shows frames can change. The profiler must
			// follow it.
			profiler::SetRenderThread(GetCurrentThreadId());

			LARGE_INTEGER start;
			QueryPerformanceCounter(&start);
			// The input read that this frame shows. It is taken at the start
			// of Present: the game can read the input of the next frame on
			// a different thread while this Present call runs, and that
			// read is for the next frame.
			const LONGLONG inputTime = g_inputTime.exchange(0);
			const std::uint64_t frameUs = MicrosBetween(g_lastPresentStart, start);
			g_lastPresentStart = start;

			// Draw calls of the frame that ends now, as an average of half a second.
			g_drawCallSum += g_drawCalls;
			g_drawCalls = 0;
			++g_drawCallFrames;
			if (MicrosBetween(g_drawCallWindow, start) >= 500000) {
				g_drawCallsPerFrame.store(static_cast<std::uint32_t>(g_drawCallSum / g_drawCallFrames));
				g_drawCallSum = 0;
				g_drawCallFrames = 0;
				g_drawCallWindow = start;
			}

			UpdateVideoMemory(start);
			UpdateWarnings(start);
			// The windows: AbyssusLib draws them at the top of its stack, and F11
			// in AbyssusLib shows or hides them. Without AbyssusLib, no windows.
			if (abyssus::HasWindows()) {
				BuildWindow();
				BuildWarnings();
			}
			const std::uint32_t gpuQueue = UpdateGpuQueue();

			LARGE_INTEGER beforePresent;
			QueryPerformanceCounter(&beforePresent);
			const HRESULT result = original(a_swapChain, a_syncInterval, a_flags);
			LARGE_INTEGER afterPresent;
			QueryPerformanceCounter(&afterPresent);

			frames::Frame frame;
			frame.frameUs = static_cast<std::uint32_t>(frameUs < 0xFFFFFFFFull ? frameUs : 0xFFFFFFFFull);
			frame.presentUs = static_cast<std::uint32_t>(MicrosBetween(beforePresent, afterPresent));
			frame.gpuQueue = gpuQueue;
			// Input read of this frame to the end of its Present call.
			if (inputTime != 0 && afterPresent.QuadPart > inputTime && g_frequency.QuadPart > 0) {
				const LONGLONG micros = (afterPresent.QuadPart - inputTime) * 1000000 / g_frequency.QuadPart;
				if (micros < 1000000) {
					frame.inputUs = static_cast<std::uint32_t>(micros);
				}
			}
			const std::uint64_t threadCpu = ThreadCpu100ns();
			if (g_lastThreadCpu100ns != 0 && threadCpu >= g_lastThreadCpu100ns) {
				frame.cpuUs = static_cast<std::uint32_t>((threadCpu - g_lastThreadCpu100ns) / 10);
			}
			g_lastThreadCpu100ns = threadCpu;
			frames::Add(frame);

			// A slow frame is a stutter that the player can see. Record it
			// with what the game thread did in it, but not more than one
			// time each second.
			const float averageMs = frames::Latest().averageMs;
			const bool  slow = frameUs >= 250000 || (frameUs >= 50000 && averageMs > 0.0f && static_cast<float>(frameUs) >= averageMs * 3000.0f);
			if (slow && MicrosBetween(g_lastLongFrameEvent, start) >= 1000000) {
				g_lastLongFrameEvent = start;
				char           what[120];
				const LONGLONG from = start.QuadPart - static_cast<LONGLONG>(frameUs) * g_frequency.QuadPart / 1000000;
				if (profiler::DescribeRange(what, sizeof(what), from, start.QuadPart)) {
					session::Event("Slow frame: %llu ms (%s)", static_cast<unsigned long long>(frameUs / 1000), what);
					if (session::SecondsSinceLoading() >= 10.0f) {
						strncpy_s(g_stutterCause, what, _TRUNCATE);
						g_stutterTime = start;
					}
				} else {
					session::Event("Slow frame: %llu ms", static_cast<unsigned long long>(frameUs / 1000));
				}
			}
			return result;
		}
	}

	void PostNote(const char* a_text, float a_seconds)
	{
		// The notification queue of AbyssusLib.
		abyssus::Notify(kSource, a_text, a_seconds);
	}

	void SetEngineState(const EngineState& a_state)
	{
		g_references.store(a_state.references);
		g_referenceLimit.store(a_state.referenceLimit);
		g_fullPlugins.store(a_state.fullPlugins);
		g_lightPlugins.store(a_state.lightPlugins);
		g_gameFullPlugins.store(a_state.gameFullPlugins);
		g_gameLightPlugins.store(a_state.gameLightPlugins);
		g_animations.store(a_state.animations);
		g_animationsKnown.store(a_state.animationsKnown);
		g_pluginWarningTime.store(a_state.pluginWarningTime);
	}

	void SetInMainMenu(bool a_inMainMenu)
	{
		g_inMainMenu.store(a_inMainMenu);
	}

	void NoteInputRead()
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		// Keep the first read of a frame.
		LONGLONG expected = 0;
		g_inputTime.compare_exchange_strong(expected, now.QuadPart);
	}

	unsigned GetDrawCallsPerFrame()
	{
		return g_drawCallsPerFrame.load();
	}

	void GetVideoMemoryMb(unsigned& a_used, unsigned& a_budget)
	{
		a_used = static_cast<unsigned>(g_vramUsed.load() >> 20);
		a_budget = static_cast<unsigned>(g_vramBudget.load() >> 20);
	}

	bool Install(void* a_swapChain, const char* a_version, const Options& a_options)
	{
		strncpy_s(g_version, a_version ? a_version : "", _TRUNCATE);
		g_options = a_options;
		g_layout = layout::Parse(a_options.layoutText ? a_options.layoutText : "");
		if (!a_swapChain || g_originalPresent.load()) {
			return false;
		}
		// The first field of a COM object points to its function table.
		void** table = *reinterpret_cast<void***>(a_swapChain);
		void** slot = &table[kPresentSlot];

		DWORD oldProtect = 0;
		if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
			return false;
		}
		g_originalPresent.store(reinterpret_cast<PresentFn>(*slot));
		InterlockedExchangePointer(slot, reinterpret_cast<void*>(&HookedPresent));
		VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
		return true;
	}
}
