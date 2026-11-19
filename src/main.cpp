#include <algorithm>
#include <cmath>
#include <ctime>
#include <optional>
#include <sstream>
#include <tuple>
#include "Config.h"
#include "Abyssus/API.h"

#include "REX/W32/OLE32.h"
#include "REX/W32/SHELL32.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include "ModMap.h"
#include "OverlayLayout.h"
#include "crash/MemTrack.h"
#include "PCH.h"

#include "GameDescribers.h"
#include "Papyrus.h"
#include "Report.h"
#include "Settings.h"
#include "crash/CrashHandler.h"
#include "crash/HookScan.h"
#include "crash/LoadTimes.h"
#include "crash/Monitor.h"
#include "crash/Phostwood.h"
#include "crash/Profiler.h"
#include "crash/Session.h"
#include "crash/SystemInfo.h"
#include "gfx/FrameStats.h"
#include "gfx/GpuTimer.h"
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

	// The SKSE log folder: Documents/My Games/<game>/SKSE. The same rule as
	// SKSE::log::log_directory() of CommonLibSSE-NG, with one change: that
	// function looks for steam_api64.dll in the current folder of the
	// process, which a launcher (Mod Organizer 2, a Proton tool) can set to
	// a different folder. Then it gives the "GOG" folder. This function
	// looks in the game folder.
	std::optional<std::filesystem::path> SkseLogDir()
	{
		static const std::optional<std::filesystem::path> dir = []() -> std::optional<std::filesystem::path> {
			wchar_t*   buffer = nullptr;
			const auto result = REX::W32::SHGetKnownFolderPath(REX::W32::FOLDERID_Documents, REX::W32::KF_FLAG_DEFAULT, nullptr, &buffer);
			std::unique_ptr<wchar_t[], decltype(&REX::W32::CoTaskMemFree)> known(buffer, REX::W32::CoTaskMemFree);
			if (!known || result != 0) {
				return std::nullopt;
			}
			std::error_code       ec;
			std::filesystem::path path = known.get();
			path /= "My Games";
			if (REL::Module::IsVR()) {
				path /= "Skyrim VR";
			} else if (std::filesystem::exists(GameDir() / "steam_api64.dll", ec)) {
				path /= std::filesystem::exists(GameDir() / "openvr_api.dll", ec) ? "Skyrim VR" : "Skyrim Special Edition";
			} else {
				path /= "Skyrim Special Edition GOG";
			}
			return path / "SKSE";
		}();
		return dir;
	}

	// The folder of all files of the tool: Data/SKSE/Plugins/AbyssusLib/SkyrimDiagnostics
	// (abyssus::ModFolder: all mods that use AbyssusLib keep their files there, one folder
	// for each mod). Log, INI, Reports, Sessions, the monitor log and the Mod Organizer 2
	// data. Earlier versions used Documents/My Games/.../SKSE/SkyrimDiagnostics.
	const std::filesystem::path& ToolFolder()
	{
		static const std::filesystem::path folder = abyssus::ModFolder("SkyrimDiagnostics");
		return folder;
	}

	// The Mod Organizer 2 data (mods.tsv, owners.tsv, profile.txt): in the tool folder; the
	// MO2 plugin of earlier versions wrote it to Data/SKSE/Plugins/SkyrimDiagnostics.
	std::filesystem::path ModMapFolder()
	{
		std::error_code ec;
		if (std::filesystem::exists(ToolFolder() / "owners.tsv", ec))
			return ToolFolder();
		const auto old = GameDir() / "Data" / "SKSE" / "Plugins" / "SkyrimDiagnostics";
		if (std::filesystem::exists(old / "owners.tsv", ec))
			return old;
		return ToolFolder();
	}

	// The log file: SkyrimDiagnostics.log in the tool folder. Made here, not by SKSE::Init.
	void SetupLog()
	{
		static std::once_flag once;
		std::call_once(once, [] {
			const auto path = ToolFolder() / "SkyrimDiagnostics.log";
			std::vector<spdlog::sink_ptr> sinks{
				std::make_shared<spdlog::sinks::msvc_sink_mt>(),
				std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true),
			};
			auto logger = std::make_shared<spdlog::logger>("global", sinks.begin(), sinks.end());
			logger->set_level(spdlog::level::info);
			logger->flush_on(spdlog::level::info);
			spdlog::set_default_logger(std::move(logger));
			spdlog::set_pattern("[%T.%e] [%=5t] [%L] %v");
		});
	}

	// The configuration file, as the game sees it.
	const std::filesystem::path& ConfigFile()
	{
		static const std::filesystem::path file = [] {
			const auto path = ToolFolder() / "SkyrimDiagnostics.ini";
			// The INI of earlier versions (Data/SKSE/Plugins/SkyrimDiagnostics.ini) is copied
			// here the first time, so the user's settings stay.
			abyssus::AdoptOldFile(GameDir() / "Data" / "SKSE" / "Plugins" / "SkyrimDiagnostics.ini", path);
			return path;
		}();
		return file;
	}
	std::string           g_layoutText;  // its [[Layout]] part
	std::vector<std::string> g_configAdded;  // what this version added to the file, for the message at the main menu

	// Reads the files that the Mod Organizer 2 plugin wrote. The game sees
	// them in its Data folder.
	void LoadModMap()
	{
		if (skydiag::modmap::Loaded()) {
			return;
		}
		const auto folder = ModMapFolder();
		if (skydiag::modmap::Load(folder)) {
			SKSE::log::info("Mod Organizer 2 data: {} files of {} mods.", skydiag::modmap::FileCount(), skydiag::modmap::ModCount());
			// profile.txt: "<profile name><tab><active mods>" (the MO2 plugin writes it at the game start).
			std::ifstream profile(folder / "profile.txt");
			std::string   line;
			if (profile && std::getline(profile, line)) {
				const auto  tab = line.find('\t');
				const auto  name = line.substr(0, tab);
				const auto  mods = tab == std::string::npos ? ""s : line.substr(tab + 1);
				const auto  text = fmt::format("Mod Organizer 2, profile \"{}\", {} active mods (list: {})", name, mods, (folder / "mods.tsv").string());
				skydiag::crash::SetSetup(text.c_str());
				SKSE::log::info("{}", text);
			}
		}
	}

	// Starts the memory count for the mod DLLs that are loaded now.
	void HookModMemory();

	skydiag::Settings g_settings;  // read from the configuration file at the start
	std::filesystem::path g_reportFolder;  // Reports in the SkyrimDiagnostics folder

	void InstallCrashHandler(const char* a_platform)
	{
		const auto logDir = SkseLogDir();
		if (!logDir) {
			SKSE::log::error("Crash handler is off: the SKSE log folder was not found.");
			return;
		}
		const auto dataDir = ToolFolder();
		const auto reportDir = dataDir / "Reports";
		std::error_code ec;
		// The session history of earlier versions (for the comparison with earlier sessions):
		// copied here one time.
		if (const auto oldSessions = *logDir / "SkyrimDiagnostics" / "Sessions";
			!std::filesystem::exists(dataDir / "Sessions", ec) && std::filesystem::exists(oldSessions, ec)) {
			std::filesystem::copy(oldSessions, dataDir / "Sessions", std::filesystem::copy_options::recursive, ec);
			SKSE::log::info("Session history copied from {} ({})", oldSessions.string(), ec ? ec.message() : "done"s);
			ec.clear();
		}
		std::filesystem::create_directories(reportDir, ec);
		if (ec) {
			SKSE::log::error("Crash handler is off: could not make the folder {}", reportDir.string());
			return;
		}

		static const std::wstring dir = reportDir.wstring();
		g_reportFolder = reportDir;
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

		config.openReport = g_settings.openReport;
		if (skydiag::crash::Install(config)) {
			SKSE::log::info("Crash handler is on. Reports: {}", reportDir.string());
		} else {
			SKSE::log::error("Crash handler could not start.");
		}

		// The monitor process: rundll32.exe with this DLL (see crash/Monitor.h).
		if (g_settings.monitor) {
			static const std::wstring skse = logDir->wstring();
			static const std::wstring monitorLog = (dataDir / "Monitor.log").wstring();
			skydiag::monitor::Options options;
			options.reportDir = dir.c_str();
			options.dataDir = data.c_str();
			options.skseDir = skse.c_str();
			options.logPath = monitorLog.c_str();
			static const std::wstring mods = (ModMapFolder() / "mods.tsv").wstring();
			options.modsFile = mods.c_str();
			options.freezeSeconds = static_cast<std::uint32_t>(g_settings.stallSeconds);
			options.openReport = g_settings.openReport;
			if (skydiag::monitor::Start(options)) {
				SKSE::log::info("Monitor process started (rundll32.exe with this DLL). Its log: {}", (dataDir / "Monitor.log").string());
			} else {
				SKSE::log::error("The monitor process could not start. Crash reports still work; dumps come from the game process.");
			}
		} else {
			SKSE::log::info("Monitor process: off in the settings file.");
		}
	}

	using Clock = std::chrono::steady_clock;

	const Clock::time_point g_pluginStart = Clock::now();

	// Heartbeat of the game update thread. The watchdog posts a request and
	// the update thread answers it. The time between the two is the response
	// time of the update thread.
	std::atomic<Clock::rep>    g_beatPostTime{ 0 };     // when the open request was posted
	std::atomic<std::uint32_t> g_lastResponseMs{ 0 };   // response time of the last answered request
	std::atomic<std::uint32_t> g_updateThreadId{ 0 };   // 0 = the update thread did not answer yet
	std::atomic<bool>          g_beatPending{ false };
	std::atomic<Clock::rep>    g_firstLoadEnd{ 0 };     // when the first cell was loaded (first loading screen after the main menu), 0 = not yet
	std::atomic<bool>          g_loading{ false };      // a loading screen is open
	std::atomic<Clock::rep>    g_loadingSince{ 0 };     // when the open loading screen opened, 0 = none
	std::string                g_saveName;             // the save that is loading or was loaded last
	std::atomic<bool>          g_dataLoaded{ false };
	skydiag::overlay::EngineState g_engineState;         // watchdog thread only

	void HookModMemory()
	{
		if (g_settings.memoryByDll) {
			const int count = skydiag::memtrack::HookModules();
			SKSE::log::info("Memory count by mod DLL: {} DLLs.", count);
		}
	}

	// The game sends its input events to this object one time in each
	// frame, when it reads the input. The time of this call is the start
	// of the input latency measurement.
	std::atomic<bool> g_captureRequest{ false };  // the capture hotkey was pressed

	class InputSink : public RE::BSTEventSink<RE::InputEvent*>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events, RE::BSTEventSource<RE::InputEvent*>*) override
		{
			skydiag::overlay::NoteInputRead();
			const int key = g_settings.captureKey;
			for (auto* e = a_events ? *a_events : nullptr; e && key != 0; e = e->next) {
				const auto* button = e->AsButtonEvent();
				if (button && e->GetDevice() == RE::INPUT_DEVICE::kKeyboard && button->IsDown() && static_cast<int>(button->GetIDCode()) == key) {
					g_captureRequest = true;
				}
			}
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
		// The "near the limit" plugin warning shows until 1 minute after the
		// first cell is loaded.
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
	// Writes the session report. A failed write is not silent: the log and
	// the events tell it one time, and again when the writes work again.
	void WriteReport()
	{
		static bool failed = false;
		if (skydiag::report::Write()) {
			if (failed) {
				failed = false;
				SKSE::log::info("Session report: the write works again.");
				skydiag::session::Event("Session report: the write works again");
			}
		} else if (const std::string error = skydiag::report::LastWriteError(); !failed && !error.empty()) {
			failed = true;
			SKSE::log::error("Session report: the write failed: {}. The tool tries again each minute.", error);
			skydiag::session::Event("Session report: the write failed: %s", error.c_str());
		}
	}

	void UpdatePapyrusTable();

	// Capture: 20 seconds at one place (the capture hotkey). Called each second
	// from the watchdog thread.
	struct Capture
	{
		bool                     active{ false };
		int                      seconds{ 0 };
		std::int64_t             startCounter{ 0 };
		std::string              place;
		std::vector<float>       fps;
		std::vector<float>       worst;
		std::vector<float>       gpu;
		std::vector<std::uint32_t> drawCalls;
	};
	Capture g_capture;

	void CaptureTick(const skydiag::report::Sample& a_sample);
	void UpdateGpuTable();
	void UpdateLoadTimeTables(bool a_log);
	void ReadPapyrusLog();

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

		const auto kLoadingLimit = std::chrono::seconds(g_settings.loadingScreenSeconds);
		bool       loadingReported = false;
		bool       slowShutdownNoted = false;
		std::uint64_t lastReadBytes = 0;
		auto       lastReadChange = Clock::now();

		int secondsSinceHookScan = 0;
		int secondsSinceLatency = 0;
		int secondsSincePapyrusLog = 0;
		for (;;) {
			std::this_thread::sleep_for(1s);
			skydiag::monitor::Beat();

			// DLLs can add hooks to the game code at any time: scan again each minute.
			if (g_dataLoaded.load() && !g_loading.load() && ++secondsSinceHookScan >= 60) {
				secondsSinceHookScan = 0;
				skydiag::crash::hooks::Scan();
			}

			// A normal game exit sets this flag. The session record must
			// know it, or the next start reports an unclean exit.
			if (!quitSeen) {
				if (const auto main = RE::Main::GetSingleton(); main && main->GetRuntimeData().quitGame) {
					quitSeen = true;
					skydiag::monitor::NoteQuit();
					skydiag::session::Event("Game exit requested");
					skydiag::session::NoteCleanExit();
					WriteReport();
				}
			}

			// Infinite loading screen: a loading screen that is open longer than
			// LoadingScreenSeconds, and no data read from the disk in the last 30 s.
			{
				const std::uint64_t readBytes = skydiag::crash::ProcessReadBytes();
				if (readBytes != lastReadBytes) {
					lastReadBytes = readBytes;
					lastReadChange = Clock::now();
				}
				const auto loadStart = g_loadingSince.load();
				if (g_loading.load() && loadStart != 0 && !quitSeen) {
					const auto open = Clock::now().time_since_epoch() - Clock::duration(loadStart);
					if (!loadingReported && open > kLoadingLimit && Clock::now() - lastReadChange > 30s) {
						loadingReported = true;
						const auto openSeconds = std::chrono::duration_cast<std::chrono::seconds>(open).count();
						skydiag::session::Event("Infinite loading screen: open for %lld s, no disk reads for 30 s", static_cast<long long>(openSeconds));
						skydiag::report::NoteProblem("stall");
						const bool ok = skydiag::crash::WriteStallReport(g_updateThreadId.load(), static_cast<std::uint32_t>(openSeconds),
							skydiag::crash::StallKind::kLoadingScreen);
						SKSE::log::warn("Loading screen open for {} s with no disk reads for 30 s. Infinite loading screen report {}.", openSeconds,
							ok ? "written" : "not written");
					}
				} else {
					loadingReported = false;
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
					skydiag::monitor::SetMainThread(g_updateThreadId.load());
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
				history.papyrusMs = scripts.valid ? scripts.vmMsPerSecond : 0.0f;
				{
					skydiag::gpu::Snapshot gpuTime;
					skydiag::gpu::GetSnapshot(gpuTime);
					history.gpuMs = gpuTime.valid ? gpuTime.frameMs : 0.0f;
				}
				history.papyrusLatencyMs = scripts.valid ? scripts.latencyMs : -1.0f;

				// Script latency test, each 5 seconds, on the game main thread.
				if (g_dataLoaded.load() && ++secondsSincePapyrusLog >= 10) {
					secondsSincePapyrusLog = 0;
					ReadPapyrusLog();
				}
				if (g_settings.papyrus && g_settings.papyrusLatency && g_dataLoaded.load() && !g_loading.load() && ++secondsSinceLatency >= 5) {
					secondsSinceLatency = 0;
					tasks->AddTask([] { skydiag::papyrus::MeasureLatency(); });
				}

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
				CaptureTick(history);

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
					UpdatePapyrusTable();
					UpdateGpuTable();
					UpdateLoadTimeTables(false);
					WriteReport();
				}

				// One time, 10 minutes into the session: compare with the
				// sessions before.
				std::string comparison;
				float       change = 0.0f;
				if (skydiag::report::CheckComparison(comparison, change)) {
					SKSE::log::info("{}", comparison);
					skydiag::session::Event("Comparison: %+.0f%% against the sessions before", change * 100.0f);
					if (change >= skydiag::report::kNotifyChange || change <= -skydiag::report::kNotifyChange) {
						if (abyssus::IsAvailable()) {
							skydiag::overlay::PostNote(comparison.c_str(), 45.0f);
						} else if (const auto hudTasks = SKSE::GetTaskInterface()) {
							// Without AbyssusLib: the game's notification, on the main thread.
							hudTasks->AddTask([text = comparison]() { RE::SendHUDMessage::ShowHUDMessage(text.c_str()); });
						}
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

			// After the player chose to quit, a long wait is a slow shutdown, not a freeze in play.
			if (quitSeen && silent > kStallLimit) {
				if (!slowShutdownNoted) {
					slowShutdownNoted = true;
					skydiag::session::Event("Slow shutdown: the game takes more than %lld s to close (no stall report)", static_cast<long long>(silentSeconds));
					SKSE::log::info("Slow shutdown: the game takes more than {} s to close. This is not a freeze in play: no stall report.", silentSeconds);
				}
				continue;
			}

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
		if (seconds > 30.0) {
			skydiag::report::NoteProblem("load-slow");
		}
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
				if (std::strcmp(menu, "Main Menu") == 0) {
					_mainMenuOpen = a_event->opening;
					skydiag::overlay::SetInMainMenu(a_event->opening);
				}
				if (std::strcmp(menu, "Main Menu") == 0 && a_event->opening && !_mainMenuSeen) {
					_mainMenuSeen = true;
					const auto sinceStart = std::chrono::duration<double>(Clock::now() - g_pluginStart).count();
					skydiag::session::Event("Game start to main menu: %.1f s", sinceStart);
					SKSE::log::info("Game start to main menu: {:.1f} s", sinceStart);
					if (sinceStart > 120.0) {
						skydiag::report::NoteProblem("load-slow");
					}
				}
				if (std::strcmp(menu, "Loading Menu") == 0) {
					if (a_event->opening) {
						_loadStart = skydiag::profiler::Now();
						_loadFromCell = CurrentCellText();
					} else if (_loadStart != 0) {
						ReportLoad("Loading screen", _loadStart, skydiag::profiler::Now(), _loadFromCell);
						_loadStart = 0;
						// The first cell: a loading screen after the main menu,
						// not one before it.
						if (g_firstLoadEnd.load() == 0 && _mainMenuSeen && !_mainMenuOpen) {
							g_firstLoadEnd = Clock::now().time_since_epoch().count();
						}
					}
					g_loading = a_event->opening;
					g_loadingSince = a_event->opening ? Clock::now().time_since_epoch().count() : 0;
					skydiag::session::SetLoading(a_event->opening);
					skydiag::monitor::SetLoading(a_event->opening);
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}

	private:
		std::int64_t _loadStart{ 0 };
		std::string  _loadFromCell;
		bool         _mainMenuSeen{ false };
		bool         _mainMenuOpen{ false };
	};

	// The SYSTEM section of crash and stall reports: the system, and the SKSE
	// plugins that SKSE did not load. No heap memory (after a crash).
	char g_systemSection[16384];

	void SystemSection(char* a_out, std::size_t a_cap)
	{
		strncpy_s(a_out, a_cap, g_systemSection, _TRUNCATE);
	}

	// At the game data load: the system data, and the SKSE log.
	void CheckSystem()
	{
		const std::string& system = skydiag::sysinfo::Collect();
		std::string        text = system;
		for (std::size_t start = 0; start < system.size();) {
			const auto end = system.find('\n', start);
			SKSE::log::info("System: {}", system.substr(start, end == std::string::npos ? std::string::npos : end - start));
			start = end == std::string::npos ? system.size() : end + 1;
		}
		std::vector<std::string> problems;
		if (const auto logDir = SkseLogDir()) {
			problems = skydiag::sysinfo::SksePluginProblems(logDir->wstring());
		}
		if (problems.empty()) {
			text += "SKSE plugins that SKSE did not load: none in the SKSE log.\n";
		} else {
			text += fmt::format("SKSE plugins that SKSE did not load or reported as not compatible ({}), from the SKSE log:\n", problems.size());
			for (const auto& line : problems) {
				text += "  " + line + "\n";
				SKSE::log::warn("SKSE log: {}", line);
			}
			skydiag::session::Event("SKSE did not load %zu plugin(s): see the SYSTEM section of the reports and the log", problems.size());
			skydiag::report::NoteProblem("skse-plugins");
			const auto note = fmt::format("SKSE did not load {} plugin DLL{}. See SkyrimDiagnostics.log (lines 'SKSE log:').", problems.size(),
				problems.size() == 1 ? "" : "s");
			skydiag::overlay::PostNote(note.c_str(), 30.0f);
		}
		if (skydiag::sysinfo::IsLoaded(L"CrashLogger.dll")) {
			text += "Crash Logger is loaded: it can take a crash before this tool. Then the crash report of this tool is missing, and the unclean "
			        "exit report has the Crash Logger log.\n";
			SKSE::log::warn("Crash Logger (CrashLogger.dll) is loaded. It can take a crash before this tool; disable it for the crash reports of this tool.");
			skydiag::overlay::PostNote("Crash Logger is loaded: it can take crashes before SkyrimDiagnostics. Disable it for the crash reports of SkyrimDiagnostics.", 20.0f);
		}
		strncpy_s(g_systemSection, text.c_str(), _TRUNCATE);
		skydiag::crash::RegisterSection("SYSTEM", &SystemSection);

		skydiag::report::Table table;
		table.title = "System";
		table.headers = { "Item" };
		for (std::size_t start = 0; start < text.size();) {
			const auto end = text.find('\n', start);
			table.rows.push_back({ text.substr(start, end == std::string::npos ? std::string::npos : end - start) });
			start = end == std::string::npos ? text.size() : end + 1;
		}
		skydiag::report::SetTable(table);
	}

#include "IniRules.inc"

	// INI checks (data/ini-rules.txt): the values that the game uses now.
	void CheckIni()
	{
		const std::string rules(reinterpret_cast<const char*>(kIniRules));
		skydiag::report::Table table;
		table.title = "INI checks";
		table.note = "Settings of Skyrim.ini and SkyrimPrefs.ini (the values that the game uses, also from mods and defaults) that a rule of this "
		             "tool names. Each rule has the date it was added to the tool: a rule can grow old.";
		table.headers = { "Setting", "Value", "Level", "What it means", "Rule added" };
		int warnings = 0;
		std::istringstream stream(rules);
		std::string        line;
		const auto value = [](const std::string& a_file, const std::string& a_name) -> std::optional<double> {
			RE::Setting* setting = nullptr;
			if (a_file == "prefs") {
				if (auto* prefs = RE::INIPrefSettingCollection::GetSingleton()) {
					setting = prefs->GetSetting(a_name);
				}
			} else if (auto* ini = RE::INISettingCollection::GetSingleton()) {
				setting = ini->GetSetting(a_name);
			}
			if (!setting) {
				return std::nullopt;
			}
			switch (setting->GetType()) {
			case RE::Setting::Type::kBool:
				return setting->GetBool() ? 1.0 : 0.0;
			case RE::Setting::Type::kFloat:
				return static_cast<double>(setting->GetFloat());
			case RE::Setting::Type::kInteger:
				return static_cast<double>(setting->GetInteger());
			case RE::Setting::Type::kUnsignedInteger:
				return static_cast<double>(setting->GetUnsignedInteger());
			default:
				return std::nullopt;
			}
		};
		while (std::getline(stream, line)) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line.empty() || line[0] == '#') {
				continue;
			}
			std::vector<std::string> parts;
			for (std::size_t start = 0;;) {
				const auto tab = line.find('\t', start);
				parts.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
				if (tab == std::string::npos) {
					break;
				}
				start = tab + 1;
			}
			if (parts.size() < 6) {
				continue;
			}
			const auto& [date, file, name, test, level, text] = std::tie(parts[0], parts[1], parts[2], parts[3], parts[4], parts[5]);
			const auto current = value(file, name);
			if (!current) {
				continue;
			}
			bool hit = false;
			if (test == "gridbuffer") {
				const auto grids = value("ini", "uGridsToLoad:General");
				hit = grids && *current < (*grids + 1.0) * (*grids + 1.0);
			} else if (test.size() > 3) {
				const std::string op = test.substr(0, 2);
				const double      limit = std::atof(test.c_str() + 3);
				hit = (op == "ne" && std::abs(*current - limit) > 1e-4) || (op == "eq" && std::abs(*current - limit) <= 1e-4) || (op == "lt" && *current < limit) ||
				      (op == "gt" && *current > limit);
			}
			if (!hit) {
				continue;
			}
			warnings += level == "warning" ? 1 : 0;
			table.rows.push_back({ name, fmt::format("{:g}", *current), level, text, date });
			SKSE::log::info("INI check ({}, rule of {}): {} = {:g}. {}", level, date, name, *current, text);
		}
		if (table.rows.empty()) {
			table.note += " No rule matches.";
		}
		if (warnings > 0) {
			skydiag::session::Event("INI checks: %d warning(s), see the table 'INI checks'", warnings);
			skydiag::report::NoteProblem("ini");
		}
		skydiag::report::SetTable(table);
	}

	// The Papyrus log (Logs/Script/Papyrus.0.log, only if Papyrus logging is on):
	// errors and warnings by script function, and stack dumps. Read each 10 s,
	// only the new part.
	struct PapyrusLogEntry
	{
		std::string function;
		int         errors{ 0 };
		int         warnings{ 0 };
		std::string example;
	};

	void ReadPapyrusLog()
	{
		static std::uintmax_t                offset = 0;
		static std::vector<PapyrusLogEntry> entries;
		static int                           stackDumps = 0;
		static bool                          started = false;
		const auto logDir = SkseLogDir();
		if (!logDir) {
			return;
		}
		const auto     path = logDir->parent_path() / "Logs" / "Script" / "Papyrus.0.log";
		std::error_code ec;
		const auto     size = std::filesystem::file_size(path, ec);
		if (ec) {
			return;
		}
		if (!started || size < offset) {
			// The game starts a new log at each start: read from the start.
			started = true;
			offset = 0;
		}
		std::ifstream file(path, std::ios::binary);
		if (!file || size <= offset) {
			return;
		}
		file.seekg(static_cast<std::streamoff>(offset));
		std::string text(static_cast<std::size_t>(std::min<std::uintmax_t>(size - offset, 8 * 1024 * 1024)), '\0');
		file.read(text.data(), static_cast<std::streamsize>(text.size()));
		text.resize(static_cast<std::size_t>(file.gcount()));
		// Only whole lines: the rest is read next time.
		const auto lastNewline = text.rfind('\n');
		if (lastNewline == std::string::npos) {
			return;
		}
		text.resize(lastNewline + 1);
		offset += text.size();

		std::istringstream stream(text);
		std::string        line;
		int                pending = 0;  // 1 error, 2 warning: waits for its first stack line
		std::string        message;
		const auto add = [&](const std::string& a_function) {
			auto it = std::find_if(entries.begin(), entries.end(), [&](const PapyrusLogEntry& a_entry) { return a_entry.function == a_function; });
			if (it == entries.end()) {
				entries.push_back({ a_function });
				it = entries.end() - 1;
			}
			(pending == 1 ? it->errors : it->warnings) += 1;
			if (it->example.empty()) {
				it->example = message.substr(0, 160);
			}
		};
		while (std::getline(stream, line)) {
			if (line.find("Suspended stack count is over our warning threshold") != std::string::npos ||
				line.find("Dumping stacks") != std::string::npos) {
				++stackDumps;
				skydiag::session::Event("Papyrus log: stack dump (the script engine has too many waiting script stacks)");
				skydiag::report::NoteProblem("papyrus-stacks");
				continue;
			}
			const auto error = line.find("] error: ");
			const auto warning = line.find("] warning: ");
			if (error != std::string::npos || warning != std::string::npos) {
				if (pending) {
					add("(no script function in the log)");
				}
				pending = error != std::string::npos ? 1 : 2;
				message = line.substr((error != std::string::npos ? error : warning) + 2);
				continue;
			}
			if (pending && line.find("()") != std::string::npos) {
				// A stack line: "[...].Script.Function() - ..." or "<unknown self>.Script.Function() - ..."
				const auto call = line.find("()");
				auto       start = line.find_last_of("]> \t", call);
				start = start == std::string::npos ? 0 : start + 1;
				std::string function = line.substr(start, call - start);
				if (!function.empty() && function[0] == '.') {
					function.erase(0, 1);
				}
				add(function.empty() ? "(not known)" : function);
				pending = 0;
			}
		}

		std::sort(entries.begin(), entries.end(), [](const PapyrusLogEntry& a_lhs, const PapyrusLogEntry& a_rhs) {
			return a_lhs.errors + a_lhs.warnings > a_rhs.errors + a_rhs.warnings;
		});
		skydiag::report::Table table;
		table.title = "Papyrus log";
		table.note = fmt::format("Errors and warnings of the script engine in this session, by the script function at the top of the stack "
		                         "(from {}). Stack dumps: {}. Many errors of one function usually come from one mod: search the script name in your mods.",
			"Logs/Script/Papyrus.0.log", stackDumps);
		table.headers = { "Script function", "Errors", "Warnings", "Example" };
		for (std::size_t i = 0; i < entries.size() && i < 30; ++i) {
			table.rows.push_back({ entries[i].function, std::to_string(entries[i].errors), std::to_string(entries[i].warnings), entries[i].example });
		}
		skydiag::report::SetTable(table);
	}

	void CaptureTick(const skydiag::report::Sample& a_sample)
	{
		constexpr int kSeconds = 20;  // the profiler keeps about 20 s of samples
		if (!g_capture.active) {
			if (!g_captureRequest.exchange(false)) {
				return;
			}
			g_capture = Capture{};
			g_capture.active = true;
			g_capture.startCounter = skydiag::profiler::Now();
			g_capture.place = CurrentCellText();
			skydiag::gpu::BeginCapture();
			skydiag::overlay::PostNote("SkyrimDiagnostics: capture of 20 s started. Play normally at this place.", 5.0f);
			skydiag::session::Event("Capture started");
			return;
		}
		g_captureRequest = false;
		if (!a_sample.loading && a_sample.active) {
			g_capture.fps.push_back(a_sample.fps);
			g_capture.worst.push_back(a_sample.worstFrameMs);
			g_capture.gpu.push_back(a_sample.gpuMs);
			g_capture.drawCalls.push_back(a_sample.drawCalls);
		}
		if (++g_capture.seconds < kSeconds) {
			return;
		}
		g_capture.active = false;

		const std::int64_t endCounter = skydiag::profiler::Now();
		const auto average = [](const auto& a_values) {
			double sum = 0;
			for (const auto v : a_values) {
				sum += v;
			}
			return a_values.empty() ? 0.0 : sum / static_cast<double>(a_values.size());
		};
		int gpuBound = 0;
		for (std::size_t i = 0; i < g_capture.fps.size(); ++i) {
			gpuBound += g_capture.gpu[i] * g_capture.fps[i] >= 900.0f ? 1 : 0;
		}
		std::string text = "SkyrimDiagnostics capture\n";
		text += fmt::format("Tool version: {}\nPlace: {}\nLength: {} s, {} s in which the game ran normally\n", SKYDIAG_VERSION,
			g_capture.place.empty() ? "not known"s : g_capture.place, kSeconds, g_capture.fps.size());
		text += "\n== FRAMES ==\n";
		text += fmt::format("Average FPS: {:.1f}. Slowest frame of each second, average: {:.0f} ms. Draw calls each frame, average: {:.0f}.\n", average(g_capture.fps),
			average(g_capture.worst), average(g_capture.drawCalls));

		static skydiag::gpu::SessionPass passes[20];
		float                            gpuAverage = 0, gpuMax = 0;
		const int                        passCount = skydiag::gpu::GetCapture(passes, static_cast<int>(std::size(passes)), gpuAverage, gpuMax);
		text += "\n== WHAT LIMITS THE FPS ==\n";
		if (gpuAverage > 0.0f && !g_capture.fps.empty()) {
			const double share = 100.0 * gpuBound / static_cast<double>(g_capture.fps.size());
			text += fmt::format("GPU time each frame: {:.1f} ms (largest second {:.1f} ms). The GPU was busy 90 % of the time or more in {:.0f} % of the seconds.\n",
				gpuAverage, gpuMax, share);
			text += share >= 50.0 ? "The GPU limits the FPS here: a lower resolution, fewer graphics effects or a faster GPU give more FPS. The render "
			                        "passes below show where the GPU time goes.\n" :
			                        "The GPU does not limit the FPS here most of the time: the processor (the game, a mod or the graphics driver), a "
			                        "frame cap or VSync does. The threads and mod DLLs below show where the processor time goes.\n";
		} else {
			text += "The GPU time is not known (GpuPasses is off, or the measurement did not start).\n";
		}
		text += "\n== GPU TIME BY RENDER PASS ==\n(by render target: size and format; passes with the same target are added)\n";
		for (int i = 0; i < passCount; ++i) {
			if (passes[i].averageMs < 0.05f) {
				break;
			}
			text += fmt::format("{:7.2f} ms  {}\n", passes[i].averageMs, passes[i].name);
		}

		text += "\n== THREADS ==\n";
		static skydiag::profiler::ThreadInfo threads[16];
		const int threadCount = skydiag::profiler::ListThreads(threads, static_cast<int>(std::size(threads)), static_cast<std::uint64_t>(kSeconds) * 1'000'000);
		for (int i = 0; i < threadCount; ++i) {
			static skydiag::profiler::Snapshot snapshot;
			skydiag::profiler::GetRange(snapshot, g_capture.startCounter, endCounter, threads[i].id);
			char describe[200] = "";
			if (!skydiag::profiler::Describe(snapshot, describe, sizeof(describe))) {
				continue;
			}
			text += fmt::format("Thread {}{}: processor use {:.0f} %, {}\n", threads[i].id,
				threads[i].isRender ? " (shows the frames)" : (threads[i].isTask ? " (game update)" : ""), threads[i].cpuShare * 100.0f, describe);
		}

		text += "\n== MOD DLLS (processor, % of one core) ==\n";
		static skydiag::profiler::ModTotal mods[12];
		const int modCount = skydiag::profiler::GetModsRange(mods, static_cast<int>(std::size(mods)), g_capture.startCounter, endCounter);
		for (int i = 0; i < modCount; ++i) {
			text += fmt::format("{:5.1f} own, {:5.1f} total  {}\n", mods[i].ownCores * 100.0f, mods[i].totalCores * 100.0f, mods[i].name);
		}

		char reads[400] = "";
		skydiag::loadtimes::SlowFileReads(g_capture.startCounter, endCounter, reads, sizeof(reads));
		text += fmt::format("\n== SLOW FILE READS (8 ms or more, the last 5) ==\n{}\n", reads[0] ? reads : "None.");

		const std::time_t now = std::time(nullptr);
		std::tm           local{};
		localtime_s(&local, &now);
		const auto path = g_reportFolder / fmt::format("capture-{:04}-{:02}-{:02}-{:02}-{:02}-{:02}.log", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
		                                       local.tm_hour, local.tm_min, local.tm_sec);
		std::ofstream file(path, std::ios::binary);
		file << text;
		const bool ok = static_cast<bool>(file);
		SKSE::log::info("Capture at {}: {}", g_capture.place, ok ? path.string() : "the file could not be written"s);
		skydiag::session::Event("Capture done: %s", ok ? path.filename().string().c_str() : "file not written");
		skydiag::overlay::PostNote(ok ? "SkyrimDiagnostics: capture done (capture-*.log in the Reports folder)." : "SkyrimDiagnostics: the capture file could not be written.",
			8.0f);
	}

	// The session table of the time in each script function.
	void UpdatePapyrusTable()
	{
		static skydiag::papyrus::FunctionTotal totals[40];
		const int                             count = skydiag::papyrus::GetFunctionTotals(totals, static_cast<int>(std::size(totals)));
		if (count == 0) {
			return;
		}
		skydiag::report::Table table;
		table.title = "Papyrus functions in this session";
		table.note = "The time that the script engine used in the code of each script function, all calls of the session together (measured, "
		             "PapyrusTime). A function with a high average or largest time is slow in each call; one with many calls is called too often.";
		table.headers = { "Function", "Total", "Calls", "Average", "Largest call" };
		for (int i = 0; i < count; ++i) {
			const auto& t = totals[i];
			table.rows.push_back({ t.name, fmt::format("{:.0f} ms", t.totalMs), std::to_string(t.calls),
				fmt::format("{:.3f} ms", t.calls ? t.totalMs / t.calls : 0.0), fmt::format("{:.2f} ms", t.maxMs) });
		}
		skydiag::report::SetTable(table);
	}

	// The session table of the GPU time of each render pass.
	void UpdateGpuTable()
	{
		static skydiag::gpu::SessionPass passes[25];
		float                            frameAverage = 0, frameMax = 0;
		const int                        count = skydiag::gpu::GetSession(passes, static_cast<int>(std::size(passes)), frameAverage, frameMax);
		if (count == 0) {
			return;
		}
		skydiag::report::Table table;
		table.title = "GPU time by render pass";
		table.note = fmt::format("GPU time of each frame: {:.1f} ms average, {:.1f} ms in the slowest second. A render pass is the work between two "
		                         "changes of the render targets; it is named by its render target (size and format). Passes with the same render target are "
		                         "added. The times are the average of the session and the largest one-second average (GpuPasses).",
			frameAverage, frameMax);
		table.headers = { "Render target", "Average each frame", "Largest" };
		for (int i = 0; i < count; ++i) {
			table.rows.push_back({ passes[i].name, fmt::format("{:.2f} ms", passes[i].averageMs), fmt::format("{:.2f} ms", passes[i].maxMs) });
		}
		skydiag::report::SetTable(table);
	}

	// The load time tables of the session report (see crash/LoadTimes.h).
	void UpdateLoadTimeTables(bool a_log)
	{
		const auto rows = skydiag::loadtimes::Rows();
		const auto ms = [](double a_ms) { return a_ms >= 1000.0 ? fmt::format("{:.1f} s", a_ms / 1000.0) : fmt::format("{:.0f} ms", a_ms); };
		const auto modOf = [](const std::string& a_dll) {
			const char* mod = skydiag::modmap::ModForDll(a_dll.c_str());
			return mod ? std::string(mod) : ""s;
		};
		const auto isStartMessage = [](const std::string& a_what) {
			return a_what == "message PostLoad" || a_what == "message PostPostLoad" || a_what == "message InputLoaded" || a_what == "message DataLoaded";
		};

		// Game start, SKSE plugins: the load function and the messages of the game start.
		struct Start
		{
			std::string dll;
			double      load{ 0 };
			double      messages{ 0 };
		};
		std::vector<Start> starts;
		for (const auto& row : rows) {
			if (row.what != "SKSEPlugin_Load" && !isStartMessage(row.what)) {
				continue;
			}
			auto it = std::find_if(starts.begin(), starts.end(), [&](const Start& a_start) { return a_start.dll == row.module; });
			if (it == starts.end()) {
				starts.push_back(Start{ row.module });
				it = starts.end() - 1;
			}
			(row.what == "SKSEPlugin_Load" ? it->load : it->messages) += row.totalMs;
		}
		std::sort(starts.begin(), starts.end(), [](const Start& a_lhs, const Start& a_rhs) { return a_lhs.load + a_lhs.messages > a_rhs.load + a_rhs.messages; });
		skydiag::report::Table table;
		table.title = "Game start: SKSE plugins";
		table.note = "The time of the load function of each SKSE plugin, and of its functions for the SKSE messages of the game start (PostLoad, "
		             "PostPostLoad, InputLoaded, DataLoaded). On Skyrim SE and VR, only plugins that load after this tool are measured.";
		table.headers = { "Mod DLL", "Mod", "Load function", "Game start messages", "Total" };
		for (const auto& start : starts) {
			if (table.rows.size() >= 40) {
				break;
			}
			table.rows.push_back({ start.dll, modOf(start.dll), ms(start.load), ms(start.messages), ms(start.load + start.messages) });
		}
		skydiag::report::SetTable(table);
		if (a_log) {
			for (std::size_t i = 0; i < starts.size() && i < 10; ++i) {
				SKSE::log::info("Game start: {} used {} (load function {}, messages {})", starts[i].dll, ms(starts[i].load + starts[i].messages),
					ms(starts[i].load), ms(starts[i].messages));
			}
		}

		// Game start, plugin files: off. The test of 0.2.0 gave all the time to one file
		// (the game does not open the plugin files one after another while it loads
		// them), so the values are not usable.
		if constexpr (false) {
		skydiag::report::Table files;
		files.title = "Game start: plugin files";
		files.note = "The approximate load time of each plugin file: from the time the game opened it to the time it opened the next one. The 40 "
		             "slowest. Long load times at the game start can also come from many loose files: see the suggestions.";
		files.headers = { "Plugin file", "Load time" };
		double filesTotal = 0;
		for (const auto& row : rows) {
			if (row.what != "plugin file") {
				continue;
			}
			filesTotal += row.totalMs;
			if (files.rows.size() < 40) {
				files.rows.push_back({ row.module, ms(row.totalMs) });
			}
		}
		if (!files.rows.empty()) {
			files.note += fmt::format(" All plugin files together: {}.", ms(filesTotal));
		}
		skydiag::report::SetTable(files);
		if (a_log && filesTotal > 0) {
			SKSE::log::info("Game start: plugin files {} in total", ms(filesTotal));
		}
		}

		// Save loads: the functions of SKSE plugins.
		skydiag::report::Table saves;
		saves.title = "Save loads: SKSE plugins";
		saves.note = "The time of the functions of SKSE plugins in save loads: the co-save load and revert functions, and the functions for the "
		             "SKSE messages PreLoadGame and PostLoadGame. All save loads of this session together.";
		saves.headers = { "Mod DLL", "Mod", "Function", "Total", "Largest", "Calls" };
		for (const auto& row : rows) {
			if (row.what != "co-save load" && row.what != "co-save revert" && row.what != "message PreLoadGame" && row.what != "message PostLoadGame") {
				continue;
			}
			if (saves.rows.size() < 40) {
				saves.rows.push_back({ row.module, modOf(row.module), row.what, ms(row.totalMs), ms(row.maxMs), std::to_string(row.count) });
			}
		}
		skydiag::report::SetTable(saves);

		// Slow file reads and opens of the whole session (8 ms or more each).
		skydiag::report::Table reads;
		reads.title = "Slow file reads";
		reads.note = "File reads and opens of the game that took 8 ms or more, by file: the files that can cause stutter. A BSA archive "
		             "holds many files; a loose file is one mesh, texture or script.";
		reads.headers = { "File", "Read or open", "Total", "Largest", "Count" };
		for (const auto& row : rows) {
			if ((row.what == "slow file read" || row.what == "slow file open") && reads.rows.size() < 40) {
				reads.rows.push_back({ row.module, row.what == "slow file open" ? "open" : "read", ms(row.totalMs), ms(row.maxMs), std::to_string(row.count) });
			}
		}
		skydiag::report::SetTable(reads);
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostLoad:
			SKSE::log::info("All SKSE plugins are loaded.");
			skydiag::crash::RefreshModules();
			{
				// DLL versions that do not work with this game version (Phostwood's data).
				static char text[4096];
				skydiag::Out out{ text, sizeof(text), 0 };
				text[0] = '\0';
				const auto version = REL::Module::get().version().string("."sv);
				const int  bad = skydiag::crash::phostwood::CheckDllVersions(version.c_str(), out);
				SKSE::log::info("Phostwood's Crash Log Analyzer data {} ({}): {} DLL{} that do{} not work well with game version {}.", skydiag::crash::phostwood::Commit(),
					skydiag::crash::phostwood::Date(), bad, bad == 1 ? "" : "s", bad == 1 ? "es" : "", version);
				if (bad > 0) {
					SKSE::log::warn("{}", std::string_view(text, out.len));
					skydiag::session::Event("DLL versions: %d DLL%s that do%s not work well with this game version (see the log)", bad, bad == 1 ? "" : "s",
						bad == 1 ? "es" : "");
				}
			}
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
			skydiag::loadtimes::NoteDataLoaded();
			UpdateLoadTimeTables(true);
			CheckSystem();
			CheckIni();
			SKSE::log::info("Game window close requests: {}.", skydiag::monitor::WatchWindowClose() ? "watched" : "not watched (window not found)");
			SKSE::log::info("Hooks of DLLs in the game code: {}.", skydiag::crash::hooks::Scan());
			// All plugins are loaded now: AbyssusLib can be found. Group "Diag", at the top.
			abyssus::SetSourceInfo("SkyrimDiagnostics", "Diag", 100);
			// All plugins are loaded now: AbyssusLib can be found.
			SKSE::log::info("AbyssusLib: {}", abyssus::HasWindows() ? "found" : (abyssus::IsAvailable() ? "found, but too old for windows" : "not found"));
			if (abyssus::VersionMismatch()) {
				SKSE::log::info("AbyssusLib: API version {}, this tool was made with version {}. Features: windows {}, notes without time limit {}.",
					abyssus::DllApiVersion(), abyssus::kApiVersion, abyssus::Has("windows") ? "yes" : "no", abyssus::Has("persistent-notes") ? "yes" : "no");
			}
			{
				// One message box at the main menu with all that the player must know.
				std::string text;
				if (!g_configAdded.empty()) {
					text += "This version added new settings to SkyrimDiagnostics.ini, with their default values:\n";
					for (const auto& added : g_configAdded) {
						text += "  " + added + "\n";
					}
				}
				if (!abyssus::HasWindows()) {
					text += (text.empty() ? "" : "\n") + std::string(abyssus::IsAvailable() ?
					                     "The installed AbyssusLib is too old. Install AbyssusLib 2 or later. The overlay does not show. The reports work." :
					                     "AbyssusLib was not found. The overlay does not show. The reports work.");
				}
				if (!text.empty()) {
					RE::DebugMessageBox(("SkyrimDiagnostics\n\n" + text).c_str());
				}
			}
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
				options.warningHoldSeconds = g_settings.warningHoldSeconds;
				options.fpsBelow = g_settings.fpsBelow;
				options.profiler = g_settings.profiler;
				options.gpuPasses = g_settings.gpuPasses;
				options.memoryByDll = g_settings.memoryByDll;
				options.layoutText = g_layoutText.c_str();
				if (skydiag::overlay::Install(window->swapChain, SKYDIAG_VERSION, options)) {
					SKSE::log::info("Frame measurement is on.");
					if (abyssus::HasWindows()) {
						SKSE::log::info("Overlay is on through AbyssusLib (F11 in AbyssusLib shows or hides it).");
					}
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
			{
				// The data is the name of the save file (no folder).
				const std::string save = a_msg->data && a_msg->dataLen > 0 ?
				                             std::string(static_cast<const char*>(a_msg->data), strnlen(static_cast<const char*>(a_msg->data), a_msg->dataLen)) :
				                             ""s;
				g_saveName = save;
				skydiag::session::Event("Save load: start (%s)", save.c_str());
				skydiag::crash::SetSave(fmt::format("{} (loading now)", save.empty() ? "name not known"s : save).c_str());
			}
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			skydiag::session::Event("Save load: end");
			UpdateLoadTimeTables(false);
			skydiag::crash::SetSave(fmt::format("{} (loaded)", g_saveName.empty() ? "name not known"s : g_saveName).c_str());
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
				const bool      made = !before && skydiag::config::WriteDefaultIfMissing(ConfigFile(), SKYDIAG_CONFIG_VERSION);
				if (made) {
					SKSE::log::info("The configuration file {} was made with the default text. With Mod Organizer 2, look for it in the overwrite folder.",
						ConfigFile().string());
				} else if (before) {
					// A file of an earlier version: add the new settings in place.
					std::string error;
					const auto  update = skydiag::config::UpdateFile(ConfigFile(), SKYDIAG_CONFIG_VERSION, error);
					if (!error.empty()) {
						SKSE::log::error("Configuration file: the update to version {} failed: {}", SKYDIAG_CONFIG_VERSION, error);
					} else if (update.changed) {
						g_configAdded = update.added;
						std::string added;
						for (const auto& item : update.added) {
							added += (added.empty() ? "" : ", ") + item;
						}
						SKSE::log::info("Configuration file: updated from version {} to {}. Added: {}", update.oldVersion.empty() ? "(none)" : update.oldVersion,
							SKYDIAG_CONFIG_VERSION, added.empty() ? "nothing" : added);
					}
				}
				if (!before && !made) {
					SKSE::log::error("The configuration file {} does not exist and the tool could not make it. The default settings are used.",
						ConfigFile().string());
				} else {
					SKSE::log::info("Configuration file: {}", ConfigFile().string());
				}
			}
			g_settings = skydiag::LoadSettings(ConfigFile().string());
			g_layoutText = skydiag::config::Load(ConfigFile()).layout;
			SKSE::log::info("Settings file {}: warning hold {} s, FPS warning below {}, profiler {}, Papyrus sampler {}, engine limits {}, animations {}, stall limit {} s",
				g_settings.fileFound ? "found" : "not found (defaults are used)", g_settings.warningHoldSeconds,
				g_settings.fpsBelow, g_settings.profiler ? "on" : "off", g_settings.papyrus ? "on" : "off", g_settings.engineLimits ? "on" : "off",
				g_settings.animations ? "on" : "off", g_settings.stallSeconds);

			SKSE::log::info("Memory count by mod DLL {}, Papyrus function time {}.", g_settings.memoryByDll ? "on" : "off",
				g_settings.papyrusTime ? "on" : "off");

			// Older versions of the tool made the log file in the SKSE
			// log folder itself. The file is now in the SkyrimDiagnostics
			// folder: remove the old one.
			if (const auto logDir = SkseLogDir()) {
				std::error_code ec;
				std::filesystem::remove(*logDir / "SkyrimDiagnostics.log", ec);
			}

			InstallCrashHandler(platform.c_str());
			// Load times: as early as possible (on AE this is the preload step,
			// before SKSE calls the load function of any plugin).
			skydiag::loadtimes::HookPluginLoad();
			skydiag::loadtimes::HookFileOpen();
			HookModMemory();
			if (!skydiag::modmap::Loaded()) {
				SKSE::log::info("Mod Organizer 2 data: not found at the start (file {}). The tool looks again when the game data is loaded.",
					(ToolFolder() / "owners.tsv").string());
			}
		});
	}
}

namespace
{
	// The log file is in the SkyrimDiagnostics folder, with the other files
	// that the tool makes. SetupLog makes it (see SkseLogDir).
	const SKSE::InitInfo kInitInfo = [] {
		SKSE::InitInfo info;
		info.log = false;
		return info;
	}();
}

// Skyrim AE only: SKSE calls this for all plugins before it calls the load
// function of any plugin. Thus the crash handler is active while the other
// plugins load.
SKSE_PLUGIN_PRELOAD(const SKSE::PreLoadInterface* a_skse)
{
	SKSE::Init(a_skse, kInitInfo);
	SetupLog();
	StartCore("preload");
	return true;
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, kInitInfo);
	SetupLog();
	StartCore("load");

	skydiag::loadtimes::HookInterfaces(SKSE::GetMessagingInterface(), SKSE::GetSerializationInterface());
	if (const auto messaging = SKSE::GetMessagingInterface(); !messaging || !messaging->RegisterListener(OnMessage)) {
		SKSE::log::error("Could not register the SKSE message listener.");
		return false;
	}

	std::thread(WatchdogLoop).detach();
	return true;
}

// The monitor process: rundll32.exe "<this DLL>",SkyDiagMonitor <game process id>.
// See crash/Monitor.h. Not called in the game process.
extern "C" __declspec(dllexport) void __stdcall SkyDiagMonitor(void*, void*, const char* a_commandLine, int)
{
	skydiag::monitor::Run(a_commandLine);
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
