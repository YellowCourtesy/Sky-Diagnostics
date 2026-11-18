#include "Config.h"
#include "ModMap.h"
#include "OverlayLayout.h"
#include "crash/MemTrack.h"
#include "PCH.h"

#include "GameDescribers.h"
#include "Papyrus.h"
#include "Report.h"
#include "Settings.h"
#include "crash/CrashHandler.h"
#include "crash/Profiler.h"
#include "crash/Session.h"
#include "gfx/FrameStats.h"
#include "gfx/Overlay.h"

namespace
{
	// Wine (and thus Proton) exports this function from its ntdll.dll.
	// Windows does not. Some OS values are emulated under Wine, so later
	// collectors must know which platform they run on.
	[[nodiscard]] const char* WineVersion() noexcept
	{
		using WineGetVersion = const char*(__cdecl*)();

		const auto ntdll = REX::W32::GetModuleHandleA("ntdll.dll");
		if (!ntdll) {
			return nullptr;
		}
		const auto fn = reinterpret_cast<WineGetVersion>(REX::W32::GetProcAddress(ntdll, "wine_get_version"));
		return fn ? fn() : nullptr;
	}

	[[nodiscard]] std::string_view RuntimeName() noexcept
	{
		if (REL::Module::IsVR()) {
			return "VR"sv;
		}
		if (REL::Module::IsAE()) {
			return "AE"sv;
		}
		return "SE"sv;
	}

	void GameStateSection(char* a_out, std::size_t a_cap);  // defined below

	// Starts the crash handler. Reports go to a folder next to the SKSE logs.
	// The folder of the game program. The paths of the tool start from it,
	// not from the current folder of the process, which a launcher can set
	// to a different folder.
	const std::filesystem::path& GameDir()
	{
		static const std::filesystem::path dir = [] {
			wchar_t path[1024] = L"";
			const auto length = REX::W32::GetModuleFileNameW(nullptr, path, static_cast<std::uint32_t>(std::size(path)));
			std::filesystem::path result = length > 0 && length < std::size(path) ? std::filesystem::path(path).parent_path() : std::filesystem::path();
			std::error_code ec;
			return result.empty() ? std::filesystem::current_path(ec) : result;
		}();
		return dir;
	}

	// The configuration file, as the game sees it.
	const std::filesystem::path& ConfigFile()
	{
		static const std::filesystem::path file = GameDir() / "Data" / "SKSE" / "Plugins" / "SkyrimDiagnostics.ini";
		return file;
	}
	std::string           g_layoutText;  // its [[Layout]] part

	// Reads the files that the Mod Organizer 2 plugin wrote. The game sees
	// them in its Data folder.
	void LoadModMap()
	{
		if (skydiag::modmap::Loaded()) {
			return;
		}
		if (skydiag::modmap::Load(GameDir() / "Data" / "SKSE" / "Plugins" / "SkyrimDiagnostics")) {
			SKSE::log::info("Mod Organizer 2 data: {} files of {} mods.", skydiag::modmap::FileCount(), skydiag::modmap::ModCount());
		}
	}

	// Starts the memory count for the mod DLLs that are loaded now.
	void HookModMemory();

	void InstallCrashHandler(const char* a_platform)
	{
		const auto logDir = SKSE::log::log_directory();
		if (!logDir) {
			SKSE::log::error("Crash handler is off: the SKSE log folder was not found.");
			return;
		}
		const auto dataDir = *logDir / "SkyrimDiagnostics";
		const auto reportDir = dataDir / "Reports";
		std::error_code ec;
		std::filesystem::create_directories(reportDir, ec);
		if (ec) {
			SKSE::log::error("Crash handler is off: could not make the folder {}", reportDir.string());
			return;
		}

		static const std::wstring dir = reportDir.wstring();
		static const std::string  game = fmt::format("Skyrim {} {}", RuntimeName(), REL::Module::get().version().string("."sv));

		skydiag::crash::Config config;
		config.reportDir = dir.c_str();
		config.toolVersion = SKYDIAG_VERSION;
		config.gameVersion = game.c_str();
		config.platform = a_platform;
		config.mainThreadId = REX::W32::GetCurrentThreadId();

		// Session record. This also makes the report for a previous session
		// that stopped with no crash report and no correct exit.
		static const std::wstring data = dataDir.wstring();
		skydiag::session::Config session;
		session.dataDir = data.c_str();
		session.reportDir = dir.c_str();
		session.toolVersion = SKYDIAG_VERSION;
		session.gameVersion = game.c_str();
		session.platform = a_platform;
		bool previousUnclean = false;
		if (skydiag::session::Start(session, &previousUnclean)) {
			if (previousUnclean) {
				SKSE::log::warn("The previous session did not end correctly. An unclean exit report was written to {}", reportDir.string());
			}
			// Session history: report file, summary, comparison with the
			// sessions before. Each event goes to the history too.
			skydiag::report::Info info;
			info.version = SKYDIAG_VERSION;
			info.game = game;
			info.platform = a_platform;
			skydiag::report::Begin(dataDir / "Sessions", info);
			skydiag::report::SetConfigFile(ConfigFile());
			skydiag::session::SetEventObserver(&skydiag::report::AddEvent);

			skydiag::session::Event("Plugin loaded");
		} else {
			SKSE::log::error("The session record could not be made.");
		}

		skydiag::RegisterGameDescribers();
		skydiag::crash::RegisterSection("GAME STATE", &GameStateSection);

		// Mod manager data: which mod each DLL and script comes from.
		skydiag::crash::SetModLookup(&skydiag::modmap::ModForDll);
		LoadModMap();

		if (skydiag::crash::Install(config)) {
			SKSE::log::info("Crash handler is on. Reports: {}", reportDir.string());
		} else {
			SKSE::log::error("Crash handler could not start.");
		}
	}

	using Clock = std::chrono::steady_clock;

	skydiag::Settings       g_settings;
	const Clock::time_point g_pluginStart = Clock::now();

	// Heartbeat of the game update thread. The watchdog posts a request and
	// the update thread answers it. The time between the two is the response
	// time of the update thread.
	std::atomic<Clock::rep>    g_beatPostTime{ 0 };     // when the open request was posted
	std::atomic<std::uint32_t> g_lastResponseMs{ 0 };   // response time of the last answered request
	std::atomic<std::uint32_t> g_updateThreadId{ 0 };   // 0 = the update thread did not answer yet
	std::atomic<bool>          g_beatPending{ false };
	std::atomic<Clock::rep>    g_firstLoadEnd{ 0 };     // when the first loading screen ended, 0 = not yet
	std::atomic<bool>          g_loading{ false };      // a loading screen is open
	std::atomic<bool>          g_dataLoaded{ false };
	skydiag::overlay::EngineState g_engineState;         // watchdog thread only

	void HookModMemory()
	{
		if (g_settings.memoryByDll) {
			const int count = skydiag::memtrack::HookModules();
			SKSE::log::info("Memory count by mod DLL: {} DLLs.", count);
		}
	}

	// The render target that the game uses for its menus and HUD now.
	void* GameUiTarget()
	{
		const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		return renderer ? static_cast<void*>(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER].RTV) : nullptr;
	}

	// The game sends its input events to this object one time in each
	// frame, when it reads the input. The time of this call is the start
	// of the input latency measurement.
	class InputSink : public RE::BSTEventSink<RE::InputEvent*>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const*, RE::BSTEventSource<RE::InputEvent*>*) override
		{
			skydiag::overlay::NoteInputRead();
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	std::string LowerCase(std::string a_text)
	{
		std::transform(a_text.begin(), a_text.end(), a_text.begin(), [](unsigned char a_char) { return static_cast<char>(std::tolower(a_char)); });
		return a_text;
	}

	// The plugin files of the game itself, in lower case: the main file,
	// the DLCs, and the Creation Club content in the file Skyrim.ccc of
	// the game folder. The game makes that file; it has the Creation Club
	// plugins that the game loads as its own content. A Creation Club
	// plugin that is not in that file counts as a mod.
	const std::vector<std::string>& GameFiles()
	{
		static const std::vector<std::string> files = [] {
			std::vector<std::string> list{ "skyrim.esm", "update.esm", "dawnguard.esm", "hearthfires.esm", "dragonborn.esm", "_resourcepack.esl" };
			std::ifstream            file(GameDir() / "Skyrim.ccc");
			std::string              line;
			std::size_t              count = 0;
			while (std::getline(file, line)) {
				while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
					line.pop_back();
				}
				if (!line.empty()) {
					list.push_back(LowerCase(line));
					++count;
				}
			}
			if (count == 0) {
				SKSE::log::warn("The file Skyrim.ccc was not found or is empty. All Creation Club plugins count as mods.");
			} else {
				SKSE::log::info("Skyrim.ccc: {} Creation Club plugins count as game files.", count);
			}
			return list;
		}();
		return files;
	}

	// Counts the object references in use and the plugins. The game has a
	// fixed table of 1,048,576 reference handles; when all are in use, the
	// game cannot make a new reference. The count reads the table with no
	// lock: a value that is some entries wrong is not important.
	void UpdateEngineState()
	{
		const auto entries = RE::BSPointerHandleManager<RE::BSUntypedPointerHandle<>>::GetHandleEntries();
		unsigned   used = 0;
		for (const auto& entry : entries) {
			used += entry.IsInUse() ? 1u : 0u;
		}
		g_engineState.references = used;
		g_engineState.referenceLimit = static_cast<unsigned>(entries.size());
		if (const auto data = RE::TESDataHandler::GetSingleton()) {
			// Plugins of the game: the main file, the DLCs and the Creation
			// Club content (the Anniversary Edition content is of this type).
			const auto isGameFile = [](const RE::TESFile* a_file) {
				if (!a_file) {
					return false;
				}
				const std::string name = LowerCase(std::string(a_file->GetFilename()));
				const auto&       known = GameFiles();
				return std::find(known.begin(), known.end(), name) != known.end();
			};
			unsigned gameFull = 0, gameLight = 0;
			const unsigned fullCount = data->GetLoadedModCount();
			const unsigned lightCount = data->GetLoadedLightModCount();
			if (const auto files = data->GetLoadedMods()) {
				for (unsigned i = 0; i < fullCount; ++i) {
					gameFull += isGameFile(files[i]) ? 1u : 0u;
				}
			}
			if (const auto files = data->GetLoadedLightMods()) {
				for (unsigned i = 0; i < lightCount; ++i) {
					gameLight += isGameFile(files[i]) ? 1u : 0u;
				}
			}
			g_engineState.fullPlugins = fullCount;
			g_engineState.lightPlugins = lightCount;
			g_engineState.gameFullPlugins = gameFull;
			g_engineState.gameLightPlugins = gameLight;
		}
		// Loaded animation files. The list is read with no lock: only its size is used.
		if (g_settings.animations) {
			if (const auto animations = RE::AnimationFileManagerSingleton::GetSingleton()) {
				g_engineState.animations = static_cast<unsigned>(animations->loadedAnimations.size());
				g_engineState.animationsKnown = true;
				// One log line when the count changes much, to check the value.
				static unsigned logged = 0xFFFFFFFF;
				if (logged == 0xFFFFFFFF || g_engineState.animations > logged + 500 || g_engineState.animations + 500 < logged) {
					logged = g_engineState.animations;
					SKSE::log::info("Animation file manager: {} loaded animations, {} in the queue, other lists {} and {}.", animations->loadedAnimations.size(),
						animations->queuedAnimations.size(), animations->unk68.size(), animations->unk88.size());
				}
				// One time, after the first loading screen: the raw data of
				// the object, to find the correct list if the count is wrong.
				static int checksAfterLoad = 0;
				if (g_firstLoadEnd.load() != 0 && ++checksAfterLoad == 2) {
					std::string  dump;
					const auto   words = reinterpret_cast<const std::uint64_t*>(animations);
					for (int i = 0; i < 0xA0 / 8; ++i) {
						dump += fmt::format("{}{:02X}:{:016X}", i == 0 ? "" : " ", i * 8, words[i]);
					}
					SKSE::log::info("Animation file manager data after the first load: {}", dump);
				}
			} else {
				static bool logged = false;
				if (!logged) {
					logged = true;
					SKSE::log::warn("The animation file manager was not found. The animation count is not available.");
				}
			}
		}
		// The plugin warning shows until 1 minute after the first loading screen.
		const auto firstLoadEnd = g_firstLoadEnd.load();
		g_engineState.pluginWarningTime = firstLoadEnd == 0 || Clock::now().time_since_epoch() - Clock::duration(firstLoadEnd) < 60s;
		skydiag::overlay::SetEngineState(g_engineState);
	}

	// The place of the player as text, for the session report. Call it on
	// a game thread only. Examples: "Dragonsreach (interior)",
	// "Whiterun (WhiterunWorld 4, -2)".
	std::string CurrentCellText()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto cell = player ? player->GetParentCell() : nullptr;
		if (!cell) {
			return {};
		}
		const char*       fullName = cell->GetFullName();
		const char*       editorId = cell->GetFormEditorID();
		const std::string name = fullName && fullName[0] ? fullName : (editorId && editorId[0] ? editorId : fmt::format("cell {:08X}", cell->GetFormID()));
		if (cell->IsInteriorCell()) {
			return name + " (interior)";
		}
		const auto  world = cell->GetRuntimeData().worldSpace;
		const char* worldName = world ? world->GetFormEditorID() : nullptr;
		if (const auto coordinates = cell->GetCoordinates()) {
			return fmt::format("{} ({} {}, {})", name, worldName && worldName[0] ? worldName : "outside", coordinates->cellX, coordinates->cellY);
		}
		return fmt::format("{} ({})", name, worldName && worldName[0] ? worldName : "outside");
	}

	// The place of the player for the crash and stall reports. A fixed
	// buffer: the report code reads it after a crash and must not use the heap.
	char g_placeText[200] = "";

	// Stores the place of the player. A change of place becomes an event,
	// so all report types show where the player was.
	void NotePlace(const std::string& a_place)
	{
		skydiag::report::SetCell(a_place.c_str());
		if (!a_place.empty() && std::strncmp(g_placeText, a_place.c_str(), sizeof(g_placeText) - 1) != 0) {
			strncpy_s(g_placeText, a_place.c_str(), _TRUNCATE);
			skydiag::session::Event("Place: %s", g_placeText);
		}
	}

	// Section for crash and stall reports: where the player was and the
	// engine values. No heap memory.
	void GameStateSection(char* a_out, std::size_t a_cap)
	{
		int used = std::snprintf(a_out, a_cap, "Place of the player: %s\n", g_placeText[0] ? g_placeText : "not known (main menu or game start)");
		if (g_engineState.referenceLimit != 0 && used > 0 && static_cast<std::size_t>(used) < a_cap) {
			used += std::snprintf(a_out + used, a_cap - used,
				"Object references in use: %u of %u (%.0f%%)\nFull plugins: %u of the game, %u others, maximum 254\nLight plugins: %u of the game, %u others, maximum 4096\n",
				g_engineState.references, g_engineState.referenceLimit,
				static_cast<double>(g_engineState.references) * 100.0 / static_cast<double>(g_engineState.referenceLimit), g_engineState.gameFullPlugins,
				g_engineState.fullPlugins - g_engineState.gameFullPlugins, g_engineState.gameLightPlugins,
				g_engineState.lightPlugins - g_engineState.gameLightPlugins);
		}
		if (g_engineState.animations != 0 && used > 0 && static_cast<std::size_t>(used) < a_cap) {
			used += std::snprintf(a_out + used, a_cap - used, "Loaded animation files: %u\n", g_engineState.animations);
		}
		if (used > 0 && static_cast<std::size_t>(used) < a_cap) {
			std::snprintf(a_out + used, a_cap - used, "Draw calls in each frame: %u\n", skydiag::overlay::GetDrawCallsPerFrame());
		}
	}

	// Finds a game update thread that does not respond.
	//
	// This thread asks SKSE to run a small task on the game update thread.
	// The task records the time. If no task runs for kStallLimit, this thread
	// makes a stall report with the call stack of each thread.
	void WatchdogLoop()
	{
		const auto kStallLimit = std::chrono::seconds(g_settings.stallSeconds);
		constexpr int  kMaxReports = 5;

		const auto tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			SKSE::log::error("Freeze detection is off: the SKSE task interface is not available.");
			return;
		}
		SKSE::log::info("Freeze detection is on (limit {} s).", kStallLimit.count());

		bool reported = false;
		bool quitSeen = false;
		bool wasForeground = true;
		int  secondsSinceWrite = 0;
		int  secondsSinceEngine = 1000;
		int  secondsSinceLoad = 0;
		int  reports = 0;

		for (;;) {
			std::this_thread::sleep_for(1s);

			// A normal game exit sets this flag. The session record must
			// know it, or the next start reports an unclean exit.
			if (!quitSeen) {
				if (const auto main = RE::Main::GetSingleton(); main && main->GetRuntimeData().quitGame) {
					quitSeen = true;
					skydiag::session::Event("Game exit requested");
					skydiag::session::NoteCleanExit();
					skydiag::report::Write();
				}
			}

			if (!g_beatPending.exchange(true)) {
				g_beatPostTime = Clock::now().time_since_epoch().count();
				tasks->AddTask([] {
					// The first answer includes the game start: do not use it.
					if (g_updateThreadId.load() != 0) {
						const auto waited = Clock::now().time_since_epoch() - Clock::duration(g_beatPostTime.load());
						g_lastResponseMs = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(waited).count());
					}
					g_updateThreadId = REX::W32::GetCurrentThreadId();
					// SKSE runs this task on the game update thread. The
					// profiler samples that thread too.
					skydiag::profiler::NoteTaskThread(g_updateThreadId.load());
					// The place of the player, for the events in the report.
					NotePlace(CurrentCellText());
					g_beatPending = false;
				});
			}

			skydiag::session::SampleInput sample;
			{
				const auto frames = skydiag::frames::Latest();
				sample.fps = frames.fps;
				sample.worstFrameMs = frames.worstMs;
				sample.frameLimit = static_cast<std::uint32_t>(frames.limit);
				sample.loading = g_loading.load();
			}

			// One sample for the session history, and the report file one
			// time each minute.
			{
				skydiag::report::Sample history;
				history.fps = sample.fps;
				history.worstFrameMs = sample.worstFrameMs;
				history.loading = sample.loading;
				history.active = skydiag::crash::IsProcessForeground();
				history.responseMs = g_lastResponseMs.load();
				skydiag::session::Latest latest;
				if (skydiag::session::GetLatest(latest)) {
					history.gameMb = static_cast<std::uint32_t>(latest.commit >> 20);
					history.gameMaxMb = static_cast<std::uint32_t>((latest.commit + latest.systemCommitFree) >> 20);
				}
				unsigned vramUsed = 0, vramBudget = 0;
				skydiag::overlay::GetVideoMemoryMb(vramUsed, vramBudget);
				history.vramMb = vramUsed;
				history.vramBudgetMb = vramBudget;
				skydiag::papyrus::Snapshot scripts;
				skydiag::papyrus::GetSnapshot(scripts);
				history.papyrusRunning = scripts.valid ? scripts.runningStacks : 0.0f;

				// Engine values: one time each 15 seconds, when the game data is loaded.
				if (g_settings.engineLimits && g_dataLoaded.load() && ++secondsSinceEngine >= 15) {
					secondsSinceEngine = 0;
					UpdateEngineState();
				}
				history.drawCalls = skydiag::overlay::GetDrawCallsPerFrame();
				history.references = g_engineState.references;
				history.referenceLimit = g_engineState.referenceLimit;
				history.animations = g_engineState.animations;
				skydiag::report::AddSample(history);

				// Processor load for the tables of the session report: one
				// period each 5 seconds, when the game runs and is not in a
				// loading screen.
				if (++secondsSinceLoad >= 5) {
					secondsSinceLoad = 0;
					if (g_settings.memoryByDll) {
						static skydiag::memtrack::Entry memory[25];
						const int memoryCount = skydiag::memtrack::GetTop(memory, static_cast<int>(std::size(memory)));
						for (int i = 0; i < memoryCount; ++i) {
							skydiag::report::SetModMemory(memory[i].name, static_cast<float>(static_cast<double>(memory[i].inUseBytes) / (1024.0 * 1024.0)),
								static_cast<float>(static_cast<double>(memory[i].allocatedBytes) / (1024.0 * 1024.0)));
						}
					}
					if (history.active && !history.loading && g_updateThreadId.load() != 0) {
						constexpr std::uint64_t kWindowUs = 5'000'000;
						static skydiag::profiler::ModTotal   mods[8];
						static skydiag::profiler::ThreadInfo threads[16];
						static skydiag::profiler::Snapshot   profile;
						const int modCount = skydiag::profiler::GetMods(mods, static_cast<int>(std::size(mods)), kWindowUs);
						for (int i = 0; i < modCount; ++i) {
							skydiag::report::AddModLoad(mods[i].name, mods[i].ownCores, mods[i].waitingCores, mods[i].totalCores);
						}
						const int threadCount = skydiag::profiler::ListThreads(threads, static_cast<int>(std::size(threads)), kWindowUs);
						for (int i = 0; i < threadCount; ++i) {
							if (threads[i].samples < 100) {
								continue;
							}
							const char* role = threads[i].isRender ? "Thread that shows the frames" :
							                   threads[i].isTask ? "Game update threads (SKSE tasks)" : "Other busy threads";
							skydiag::profiler::GetSnapshot(profile, kWindowUs, threads[i].id);
							for (int e = 0; e < profile.count; ++e) {
								skydiag::report::AddThreadWork(role, profile.entries[e].name, profile.entries[e].share);
							}
						}
						if (scripts.valid) {
							for (int i = 0; i < scripts.count; ++i) {
								skydiag::report::AddScriptLoad(scripts.entries[i].name, scripts.entries[i].running, scripts.entries[i].waiting);
							}
							for (int i = 0; i < scripts.functionCount; ++i) {
								skydiag::report::AddFunctionLoad(scripts.functions[i].name, scripts.functions[i].running);
							}
							for (int i = 0; i < scripts.timedCount; ++i) {
								skydiag::report::AddFunctionTime(scripts.timed[i].name, scripts.timed[i].msPerSecond, scripts.timed[i].callsPerSecond);
							}
						}
						skydiag::report::EndLoadPeriod();
					}
				}

				if (++secondsSinceWrite >= 60) {
					secondsSinceWrite = 0;
					skydiag::report::Write();
				}

				// One time, 10 minutes into the session: compare with the
				// sessions before.
				std::string comparison;
				float       change = 0.0f;
				if (skydiag::report::CheckComparison(comparison, change)) {
					SKSE::log::info("{}", comparison);
					skydiag::session::Event("Comparison: %+.0f%% against the sessions before", change * 100.0f);
					if (change >= skydiag::report::kNotifyChange || change <= -skydiag::report::kNotifyChange) {
						skydiag::overlay::PostNote(comparison.c_str(), 45.0f);
					}
				}
			}

			if (g_updateThreadId.load() == 0) {
				skydiag::session::Sample(sample);
				continue;  // the game update loop did not start yet
			}

			// The game stops its update loop while its window is not active.
			// That is not a freeze: the waiting time starts again when the
			// window is active again.
			const bool foreground = skydiag::crash::IsProcessForeground();
			if (foreground != wasForeground) {
				wasForeground = foreground;
				skydiag::session::Event(foreground ? "Game window: active" : "Game window: not active");
			}
			if (!foreground) {
				g_beatPostTime = Clock::now().time_since_epoch().count();
				skydiag::session::Sample(sample);
				continue;
			}

			// Time that the open request waits. Zero if the update thread
			// answered the last request.
			const auto silent = g_beatPending.load() ? Clock::now().time_since_epoch() - Clock::duration(g_beatPostTime.load()) : Clock::duration::zero();
			const auto silentSeconds = std::chrono::duration_cast<std::chrono::seconds>(silent).count();
			const auto silentMs = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(silent).count());
			sample.updateResponseMs = silentMs > 1000 ? silentMs : g_lastResponseMs.load();
			skydiag::session::Sample(sample);

			if (!reported && silent > kStallLimit) {
				reported = true;
				skydiag::session::Event("Stall: the game update thread did not respond for %lld s", static_cast<long long>(silentSeconds));
				skydiag::report::NoteProblem("stall");
				if (reports < kMaxReports) {
					++reports;
					const bool ok = skydiag::crash::WriteStallReport(g_updateThreadId.load(), static_cast<std::uint32_t>(silentSeconds));
					SKSE::log::warn("The game update thread did not respond for {} s. Stall report {}.", silentSeconds,
						ok ? "written" : "not written");
				} else {
					SKSE::log::warn("The game update thread did not respond for {} s. No more stall reports in this session (limit {}).",
						silentSeconds, kMaxReports);
				}
			} else if (reported && silent < 5s) {
				reported = false;
				skydiag::session::Event("The game update thread responds again");
				SKSE::log::info("The game update thread responds again.");
			}
		}
	}

	// The character of the save that is loaded. The session report goes to
	// the folder of this character and is compared only with its sessions.
	void NoteCharacter()
	{
		const auto  player = RE::PlayerCharacter::GetSingleton();
		const char* name = player ? player->GetName() : nullptr;
		if (name && name[0] && skydiag::report::SetGroup(name)) {
			skydiag::session::Event("Character: %s", name);
			SKSE::log::info("Character: {}. The session report is in the folder of this character.", name);
		}
	}

	// Writes how long a loading screen took and what used the processors
	// in it. The profiler keeps about 20 seconds of samples, so for a long
	// load the breakdown covers the last 15 seconds.
	void ReportLoad(const char* a_what, std::int64_t a_fromCounter, std::int64_t a_toCounter, const std::string& a_fromCell)
	{
		// The player is now in the place that was loaded.
		const std::string toCell = CurrentCellText();
		NotePlace(toCell);

		const std::int64_t ticks = skydiag::profiler::TicksPerSecond();
		const double       seconds = static_cast<double>(a_toCounter - a_fromCounter) / static_cast<double>(ticks);
		const std::int64_t from = (std::max)(a_fromCounter, a_toCounter - 15 * ticks);

		std::string text = fmt::format("{}: {:.1f} s", a_what, seconds);
		if (!toCell.empty()) {
			text += a_fromCell.empty() || a_fromCell == toCell ? fmt::format(", to {}", toCell) : fmt::format(", from {} to {}", a_fromCell, toCell);
		}
		char        busiest[160] = "";
		if (skydiag::profiler::DescribeRange(busiest, sizeof(busiest), from, a_toCounter)) {
			text += fmt::format(". Busiest thread: {}", busiest);
		}
		skydiag::profiler::ModTotal mods[3];
		const int                   count = skydiag::profiler::GetModsRange(mods, 3, from, a_toCounter);
		bool                        first = true;
		for (int i = 0; i < count; ++i) {
			const float work = mods[i].totalCores - mods[i].waitingCores;
			if (work < 0.05f) {
				continue;
			}
			text += fmt::format("{}{} {:.0f}% of one core", first ? ". Mod DLLs at work: " : ", ", mods[i].name, work * 100.0f);
			first = false;
		}
		skydiag::session::Event("%s", text.c_str());
		SKSE::log::info("{}", text);
	}

	// Records each menu that opens or closes. The loading screen is a menu,
	// so the event list shows when a load started and ended.
	class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event) {
				skydiag::session::Event("Menu %s: %s", a_event->opening ? "open" : "close", a_event->menuName.c_str());
				const char* menu = a_event->menuName.c_str();
				if (std::strcmp(menu, "Main Menu") == 0 && a_event->opening && !_mainMenuSeen) {
					_mainMenuSeen = true;
					const auto sinceStart = std::chrono::duration<double>(Clock::now() - g_pluginStart).count();
					skydiag::session::Event("Game start to main menu: %.1f s", sinceStart);
					SKSE::log::info("Game start to main menu: {:.1f} s", sinceStart);
				}
				if (std::strcmp(menu, "Loading Menu") == 0) {
					if (a_event->opening) {
						_loadStart = skydiag::profiler::Now();
						_loadFromCell = CurrentCellText();
					} else if (_loadStart != 0) {
						ReportLoad("Loading screen", _loadStart, skydiag::profiler::Now(), _loadFromCell);
						_loadStart = 0;
						if (g_firstLoadEnd.load() == 0) {
							g_firstLoadEnd = Clock::now().time_since_epoch().count();
						}
					}
					g_loading = a_event->opening;
					skydiag::session::SetLoading(a_event->opening);
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}

	private:
		std::int64_t _loadStart{ 0 };
		std::string  _loadFromCell;
		bool         _mainMenuSeen{ false };
	};

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostLoad:
			SKSE::log::info("All SKSE plugins are loaded.");
			skydiag::crash::RefreshModules();
			HookModMemory();
			break;
		case SKSE::MessagingInterface::kInputLoaded:
			if (const auto input = RE::BSInputDeviceManager::GetSingleton()) {
				static InputSink inputSink;
				input->AddEventSink(&inputSink);
				SKSE::log::info("Input latency measurement is on.");
			}
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			SKSE::log::info("Game data is loaded.");
			skydiag::session::Event("Game data loaded");
			g_dataLoaded = true;
			LoadModMap();
			HookModMemory();
			if (const auto ui = RE::UI::GetSingleton()) {
				static MenuSink menuSink;
				ui->AddEventSink<RE::MenuOpenCloseEvent>(&menuSink);
			}
			// Frame measurement and overlay: hook the Present function of
			// the game's swap chain.
			if (const auto window = RE::BSGraphics::Renderer::GetCurrentRenderWindow(); window && window->swapChain) {
				skydiag::overlay::Options options;
				options.view = g_settings.view;
				options.toggleKey = g_settings.toggleKey;
				options.warningHoldSeconds = g_settings.warningHoldSeconds;
				options.fpsBelow = g_settings.fpsBelow;
				options.profiler = g_settings.profiler;
				options.memoryByDll = g_settings.memoryByDll;
				options.gameTarget = g_settings.overlayOnGameTarget ? &GameUiTarget : nullptr;
				options.layoutText = g_layoutText.c_str();
				if (skydiag::overlay::Install(window->swapChain, SKYDIAG_VERSION, options)) {
					SKSE::log::info("Frame measurement and overlay are on (key 0x{:X} changes the view: all data, warnings only, off).", g_settings.toggleKey);
				} else {
					SKSE::log::error("Frame measurement is off: the Present hook failed.");
				}
			} else {
				SKSE::log::error("Frame measurement is off: the swap chain was not found.");
			}
			if (g_settings.papyrus) {
				skydiag::papyrus::Start(g_settings.papyrusTime);
			} else {
				SKSE::log::info("Papyrus sampler is off in the settings file.");
			}
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			skydiag::session::Event("Save load: start");
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			skydiag::session::Event("Save load: end");
			NoteCharacter();
			break;
		case SKSE::MessagingInterface::kSaveGame:
			skydiag::session::Event("Save game");
			// A new game has its character name at the first save.
			NoteCharacter();
			break;
		case SKSE::MessagingInterface::kNewGame:
			skydiag::session::Event("New game");
			break;
		default:
			break;
		}
	}
}

namespace
{
	// Starts the parts that do not need the game: log lines, session record,
	// crash handler. Runs one time, at the first entry point that SKSE calls.
	void StartCore(const char* a_entryPoint)
	{
		static std::once_flag once;
		std::call_once(once, [&] {
			SKSE::log::info("{} {} started at {}.", SKSE::GetPluginName(), SKYDIAG_VERSION, a_entryPoint);
			SKSE::log::info("Game runtime: Skyrim {} {}", RuntimeName(), REL::Module::get().version().string("."sv));

			static const std::string platform = [] {
				const auto wine = WineVersion();
				return wine ? fmt::format("Wine {} (Proton or equivalent)", wine) : "Windows"s;
			}();
			SKSE::log::info("Platform: {}", platform);

			// The tool makes the configuration file if there is none.
			SKSE::log::info("Game folder: {}", GameDir().string());
			{
				std::error_code ec;
				const bool      before = std::filesystem::exists(ConfigFile(), ec);
				const bool      made = skydiag::config::WriteDefaultIfMissing(ConfigFile());
				if (made) {
					SKSE::log::info("The configuration file {} was made with the default text. With Mod Organizer 2, look for it in the overwrite folder.",
						ConfigFile().string());
				} else if (!before) {
					SKSE::log::error("The configuration file {} does not exist and the tool could not make it. The default settings are used.",
						ConfigFile().string());
				} else {
					SKSE::log::info("Configuration file: {}", ConfigFile().string());
				}
			}
			g_settings = skydiag::LoadSettings(ConfigFile().string());
			g_layoutText = skydiag::config::Load(ConfigFile()).layout;
			SKSE::log::info("Settings file {}: view {}, view key 0x{:X}, warning hold {} s, FPS warning below {}, profiler {}, Papyrus sampler {}, engine limits {}, animations {}, stall limit {} s",
				g_settings.fileFound ? "found" : "not found (defaults are used)", g_settings.view, g_settings.toggleKey, g_settings.warningHoldSeconds,
				g_settings.fpsBelow, g_settings.profiler ? "on" : "off", g_settings.papyrus ? "on" : "off", g_settings.engineLimits ? "on" : "off",
				g_settings.animations ? "on" : "off", g_settings.stallSeconds);

			SKSE::log::info("Memory count by mod DLL {}, Papyrus function time {}, overlay target: {}.", g_settings.memoryByDll ? "on" : "off",
				g_settings.papyrusTime ? "on" : "off", g_settings.overlayOnGameTarget ? "game UI target" : "swap chain");

			// Older versions of the tool made the log file in the SKSE
			// log folder itself. The file is now in the SkyrimDiagnostics
			// folder: remove the old one.
			if (const auto logDir = SKSE::log::log_directory()) {
				std::error_code ec;
				std::filesystem::remove(*logDir / "SkyrimDiagnostics.log", ec);
			}

			InstallCrashHandler(platform.c_str());
			HookModMemory();
			if (!skydiag::modmap::Loaded()) {
				SKSE::log::info("Mod Organizer 2 data: not found at the start (file {}). The tool looks again when the game data is loaded.",
					(GameDir() / "Data" / "SKSE" / "Plugins" / "SkyrimDiagnostics" / "owners.tsv").string());
			}
		});
	}
}

namespace
{
	// The log file is in the SkyrimDiagnostics folder, with the other files
	// that the tool makes.
	const SKSE::InitInfo kInitInfo = [] {
		SKSE::InitInfo info;
		info.logName = "SkyrimDiagnostics/SkyrimDiagnostics";
		return info;
	}();
}

// Skyrim AE only: SKSE calls this for all plugins before it calls the load
// function of any plugin. Thus the crash handler is active while the other
// plugins load.
SKSE_PLUGIN_PRELOAD(const SKSE::PreLoadInterface* a_skse)
{
	SKSE::Init(a_skse, kInitInfo);
	StartCore("preload");
	return true;
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, kInitInfo);
	StartCore("load");

	if (const auto messaging = SKSE::GetMessagingInterface(); !messaging || !messaging->RegisterListener(OnMessage)) {
		SKSE::log::error("Could not register the SKSE message listener.");
		return false;
	}

	std::thread(WatchdogLoop).detach();
	return true;
}

// The loader calls this when the process exits through the normal exit path.
// A process that is stopped from outside, or that crashes, does not get here.
extern "C" int __stdcall DllMain(void*, unsigned long a_reason, void*)
{
	constexpr unsigned long kProcessDetach = 0;
	if (a_reason == kProcessDetach) {
		skydiag::session::NoteCleanExit();
	}
	return 1;
}
