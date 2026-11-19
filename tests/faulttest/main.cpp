// SkyrimDiagnosticsFaultTest: a separate SKSE plugin that makes faults on
// purpose, so that the detection of SkyrimDiagnostics can be tested with a
// real mod DLL. Do not install it for normal play.
//
// It reads the file inject.txt in Data/SKSE/Plugins/AbyssusLib/SkyrimDiagnostics
// (the folder of all SkyrimDiagnostics files) every 2 seconds. Each line is a word and a number:
//   cpu 10     use the processor for 10 ms in each frame, on the game thread
//   sleep 20   wait for 20 ms in each frame, on the game thread
//   leak 600   allocate 600 MB each minute and never release it
//   crash      crash the game one time, on a game thread
//   freeze     stop a game update thread and never continue
// An empty file stops the cpu, sleep and leak faults. Memory that was
// allocated stays allocated until the game ends.
//
// The plugin makes an empty inject.txt if the file does not exist. Before a
// crash or a freeze it makes the file empty, so that the next game start
// does not do the fault again.

#include "../../src/PCH.h"

#include "../../src/Abyssus/API.h"

#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>

namespace
{
	std::atomic<int>  g_cpuMs{ 0 };
	std::atomic<int>  g_sleepMs{ 0 };
	std::atomic<int>  g_leakMbPerMinute{ 0 };
	std::atomic<bool> g_taskPending{ false };

	using Clock = std::chrono::steady_clock;

	// Uses the processor in this DLL's own code for the given time.
	__declspec(noinline) std::uint64_t Burn(int a_milliseconds)
	{
		volatile std::uint64_t value = 0;
		const auto             end = Clock::now() + std::chrono::milliseconds(a_milliseconds);
		while (Clock::now() < end) {
			for (std::uint64_t i = 0; i < 20000; ++i) {
				value = value + i;
			}
		}
		return value;
	}

	std::filesystem::path InjectPath()
	{
		return abyssus::ModFolder("SkyrimDiagnostics") / "inject.txt";
	}

	// Makes the file empty (and makes it if it does not exist).
	void ClearFile(const std::filesystem::path& a_path)
	{
		std::error_code ec;
		std::filesystem::create_directories(a_path.parent_path(), ec);
		std::ofstream file(a_path, std::ios::trunc);
	}

	__declspec(noinline) void Crash()
	{
		// Keeps a pointer to a known form (the player base record, FormID
		// 00000007 from Skyrim.esm) on the stack. The crash report must find
		// it and show its data. This tests the form describer.
		RE::TESForm* volatile knownForm = RE::TESForm::LookupByID(0x7);
		volatile int*         target = nullptr;
		*target = knownForm ? 1 : 2;
	}

	void ReadSettings()
	{
		int  cpu = 0, sleep = 0, leak = 0;
		bool crash = false, freeze = false;
		const auto path = InjectPath();
		if (path.empty()) {
			return;
		}
		{
			std::ifstream file(path);
			std::string   line;
			while (std::getline(file, line)) {
				std::istringstream words(line);
				std::string        word;
				int                number = 0;
				if (!(words >> word)) {
					continue;
				}
				words >> number;
				if (word == "cpu") {
					cpu = std::clamp(number, 0, 500);
				} else if (word == "sleep") {
					sleep = std::clamp(number, 0, 500);
				} else if (word == "leak") {
					leak = std::clamp(number, 0, 20000);
				} else if (word == "crash") {
					crash = true;
				} else if (word == "freeze") {
					freeze = true;
				} else {
					SKSE::log::error("inject.txt has an unknown word: '{}'. Use cpu, sleep, leak, crash or freeze.", word);
				}
			}
		}
		if (cpu != g_cpuMs || sleep != g_sleepMs || leak != g_leakMbPerMinute) {
			SKSE::log::info("Settings: cpu {} ms each frame, sleep {} ms each frame, leak {} MB each minute", cpu, sleep, leak);
		}
		g_cpuMs = cpu;
		g_sleepMs = sleep;
		g_leakMbPerMinute = leak;

		if (crash || freeze) {
			const auto tasks = SKSE::GetTaskInterface();
			if (!tasks) {
				SKSE::log::error("The crash or freeze was not started: the SKSE task interface is not available.");
				return;
			}
			// One time only: the file is empty before the fault occurs.
			ClearFile(path);
			g_cpuMs = 0;
			g_sleepMs = 0;
			g_leakMbPerMinute = 0;
			if (crash) {
				SKSE::log::warn("Test crash: the game now crashes on purpose.");
				spdlog::default_logger()->flush();
				tasks->AddTask([] { Crash(); });
			} else {
				SKSE::log::warn("Test freeze: a game update thread now stops. Stop the game after the stall report is written.");
				spdlog::default_logger()->flush();
				tasks->AddTask([] {
					for (;;) {
						std::this_thread::sleep_for(1s);
					}
				});
			}
		}
	}

	void Loop()
	{
		const auto tasks = SKSE::GetTaskInterface();
		auto       lastRead = Clock::now() - 10s;
		auto       lastLeak = Clock::now();

		for (;;) {
			std::this_thread::sleep_for(1ms);
			const auto now = Clock::now();

			if (now - lastRead >= 2s) {
				lastRead = now;
				ReadSettings();
			}

			// One task for each frame: SKSE runs the tasks on the game
			// thread one time in each frame.
			if (tasks && (g_cpuMs > 0 || g_sleepMs > 0) && !g_taskPending.exchange(true)) {
				tasks->AddTask([] {
					if (const int cpu = g_cpuMs; cpu > 0) {
						Burn(cpu);
					}
					if (const int sleep = g_sleepMs; sleep > 0) {
						std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
					}
					g_taskPending = false;
				});
			}

			if (now - lastLeak >= 1s) {
				lastLeak = now;
				if (const int leak = g_leakMbPerMinute; leak > 0) {
					const std::size_t bytes = static_cast<std::size_t>(leak) * 1024 * 1024 / 60;
					if (char* block = new (std::nothrow) char[bytes]) {
						std::memset(block, 1, bytes);  // use the memory, so that it is real
					}
				}
			}
		}
	}
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	// The log file is in Data/SKSE/Plugins/AbyssusLib/SkyrimDiagnostics, with all other
	// files of SkyrimDiagnostics. Older versions made it in the SKSE log folder (also in its
	// SkyrimDiagnostics folder): remove those files.
	SKSE::InitInfo info;
	info.log = false;
	SKSE::Init(a_skse, info);
	{
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((abyssus::ModFolder("SkyrimDiagnostics") / "SkyrimDiagnosticsFaultTest.log").string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%T.%e] [%=5t] [%L] %v");
	}
	if (const auto logDir = SKSE::log::log_directory()) {
		std::error_code ec;
		std::filesystem::remove(*logDir / "SkyrimDiagnosticsFaultTest.log", ec);
		std::filesystem::remove(*logDir / "SkyrimDiagnostics" / "SkyrimDiagnosticsFaultTest.log", ec);
	}
	SKSE::log::info("SkyrimDiagnosticsFaultTest loaded. This plugin makes faults on purpose. Remove it for normal play.");
	// An empty command file, so that the user must not make it.
	if (const auto path = InjectPath(); !path.empty()) {
		std::error_code ec;
		if (!std::filesystem::exists(path, ec)) {
			ClearFile(path);
		}
	}
	std::thread(Loop).detach();
	return true;
}
