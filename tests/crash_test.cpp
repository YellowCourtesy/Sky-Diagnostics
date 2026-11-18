// Test program that plays the role of the game. It installs the crash
// handler and then crashes in the way that the first argument selects.

#include "../src/crash/CrashHandler.h"
#include "../src/crash/MemTrack.h"
#include "../src/crash/Profiler.h"
#include "../src/crash/Session.h"
#include "../src/gfx/Advisor.h"
#include "../src/gfx/FrameStats.h"
#include "../src/Report.h"
#include "../src/Settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

extern "C" __declspec(dllimport) void BadModNullWrite();
extern "C" __declspec(dllimport) void BadModThrow();
extern "C" __declspec(dllimport) int  BadModOverflow();
extern "C" __declspec(dllimport) void BadModSpin();
extern "C" __declspec(dllimport) unsigned long BadModBlock();

extern "C" __declspec(dllimport) void BadModGameObject();
extern "C" __declspec(dllimport) void* BadModAllocate(unsigned long long a_bytes, int a_useHeap);
extern "C" __declspec(dllimport) void  BadModRelease(void* a_block, int a_useHeap);
extern "C" __declspec(dllimport) unsigned long long BadModSpinFor(unsigned long a_milliseconds);
extern "C" __declspec(dllimport) unsigned long BadModSleepFor(unsigned long a_milliseconds);

namespace
{
	// Same layout as the start of FakeForm in badmod.cpp.
	struct FakeFormView
	{
		void*        vtable;
		unsigned int id;
	};

	void DescribeFakeForm(void* a_subobject, char* a_text, std::size_t a_textCap, char* a_source, std::size_t a_sourceCap)
	{
		const auto form = static_cast<const FakeFormView*>(a_subobject);
		std::snprintf(a_text, a_textCap, "FakeForm id 0x%X", form->id);
		std::snprintf(a_source, a_sourceCap, "TestPlugin.esp");
	}

	// Plays the role of the watchdog: after a short time it makes a stall
	// report for the main thread and stops the process with exit code 42.
	DWORD WINAPI StallWatcher(LPVOID a_mainThreadId)
	{
		Sleep(1500);
		const bool ok = skydiag::crash::WriteStallReport(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(a_mainThreadId)), 30);
		ExitProcess(ok ? 42 : 3);
	}

	void StartStallWatcher()
	{
		CreateThread(nullptr, 0, &StallWatcher, reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(GetCurrentThreadId())), 0, nullptr);
	}
}

int wmain(int argc, wchar_t** argv)
{
	if (argc < 3) {
		std::fprintf(stderr, "usage: crash_test <mode> <report dir>\n");
		return 2;
	}

	// No error dialogs: the test runs without a user.
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

	skydiag::crash::Config config;
	config.reportDir = argv[2];
	config.toolVersion = "test";
	config.gameVersion = "crash_test";
	config.platform = "test";
	config.mainThreadId = GetCurrentThreadId();
	// Session record in the same folder. The previous run of this program
	// in this folder is the "previous session".
	skydiag::session::Config session;
	session.dataDir = argv[2];
	session.reportDir = argv[2];
	session.toolVersion = "test";
	session.gameVersion = "crash_test";
	session.platform = "test";
	bool previousUnclean = false;
	if (!skydiag::session::Start(session, &previousUnclean)) {
		std::fprintf(stderr, "Session start failed\n");
		return 2;
	}
	skydiag::session::Event("test event %s", "one");
	skydiag::session::Sample({});

	skydiag::crash::RegisterDescriber("FakeForm", &DescribeFakeForm);
	skydiag::crash::RegisterSection("TEST SECTION", [](char* a_out, std::size_t a_cap) { std::snprintf(a_out, a_cap, "section text 42\n"); });
	if (!skydiag::crash::Install(config)) {
		std::fprintf(stderr, "Install failed\n");
		return 2;
	}

	// Mod manager data: the test DLL comes from the mod "Test Mod".
	skydiag::crash::SetModLookup([](const char* a_dll) -> const char* { return _stricmp(a_dll, "badmod.dll") == 0 ? "Test Mod" : ""; });
	// Memory count by mod DLL. The test DLL keeps 3 MB for all modes.
	const int   trackedDlls = skydiag::memtrack::HookModules();
	void* const keptBlock = BadModAllocate(3 * 1024 * 1024, 0);
	(void)keptBlock;

	const wchar_t* mode = argv[1];
	if (std::wcscmp(mode, L"memtrack") == 0) {
		using namespace skydiag::memtrack;
		const double kMb = 1024.0 * 1024.0;
		const auto   inUse = [&]() {
			  Entry entries[8];
			  const int count = GetTop(entries, 8);
			  for (int i = 0; i < count; ++i) {
				  if (_stricmp(entries[i].name, "badmod.dll") == 0) {
					  return static_cast<double>(entries[i].inUseBytes) / kMb;
				  }
			  }
			  return -1.0;
		};
		const double start = inUse();
		void* const  a = BadModAllocate(8 * 1024 * 1024, 0);
		void* const  b = BadModAllocate(4 * 1024 * 1024, 1);
		const double both = inUse();
		BadModRelease(a, 0);
		const double one = inUse();
		BadModRelease(b, 1);
		const double end = inUse();
		// A second call adds no DLL two times.
		const int  again = HookModules();
		const bool ok = trackedDlls >= 1 && again == trackedDlls && start >= 3.0 && start < 3.6 && both - start > 11.9 && both - start < 12.5 &&
		                one - start > 3.9 && one - start < 4.5 && end - start > -0.3 && end - start < 0.3;
		std::printf("memtrack: %d DLLs, in use %.2f, %.2f, %.2f, %.2f MB: %s\n", trackedDlls, start, both, one, end, ok ? "ok" : "wrong");
		skydiag::session::NoteCleanExit();
		return ok ? 80 : 81;
	}
	if (std::wcscmp(mode, L"session-next") == 0) {
		// Reports what the start of this session found.
		skydiag::session::NoteCleanExit();
		return previousUnclean ? 10 : 11;
	}
	if (std::wcscmp(mode, L"profile") == 0) {
		using namespace skydiag::profiler;
		// This thread plays the role of the thread that shows the frames.
		// It works in the test mod for 1.5 s and then sleeps for 0.7 s.
		Start(GetCurrentThreadId());
		Sleep(100);
		LARGE_INTEGER spinStart, spinEnd;
		QueryPerformanceCounter(&spinStart);
		BadModSpinFor(1500);
		QueryPerformanceCounter(&spinEnd);
		Sleep(700);

		Snapshot all;
		GetSnapshot(all, 2200000);
		Snapshot spin;
		GetRange(spin, spinStart.QuadPart, spinEnd.QuadPart);
		char text[200] = "";
		Describe(all, text, sizeof(text));
		std::printf("profile: %u samples, %s, cost %.2f%%\n", all.samples, text, all.costShare * 100.0f);

		const bool modFirst = all.count > 0 && std::strcmp(all.entries[0].name, "badmod.dll") == 0 && all.entries[0].isMod;
		const bool sharesOk = modFirst && all.entries[0].share > 0.45f && all.entries[0].share < 0.85f && all.waitShare > 0.15f && all.waitShare < 0.5f &&
		                      all.modCount > 0 && std::strcmp(all.mods[0].name, "badmod.dll") == 0 && all.mods[0].ownCode > 0.45f &&
		                      all.mods[0].waiting < 0.05f;
		const bool rangeOk = spin.count > 0 && std::strcmp(spin.entries[0].name, "badmod.dll") == 0 && spin.entries[0].share > 0.9f;
		char rangeText[200] = "";
		const bool describeOk = DescribeRange(rangeText, sizeof(rangeText), spinStart.QuadPart, spinEnd.QuadPart) &&
		                        std::strstr(rangeText, "badmod.dll") != nullptr;

		// A thread that the profiler was not told about works in the test
		// mod for 4 s, while this thread sleeps. The profiler must find it
		// by its processor use and name the mod.
		DWORD        workerId = 0;
		const HANDLE worker = CreateThread(
			nullptr, 0,
			[](LPVOID) -> DWORD {
				BadModSpinFor(4000);
				return 0;
			},
			nullptr, 0, &workerId);
		Sleep(3900);
		ThreadInfo threads[16];
		const int  threadCount = ListThreads(threads, 16, 1000000);
		bool       workerFound = false;
		for (int i = 0; i < threadCount; ++i) {
			workerFound = workerFound || (threads[i].id == workerId && threads[i].waitShare < 0.2f && !threads[i].isRender);
		}
		Snapshot onWorker;
		GetSnapshot(onWorker, 1000000, workerId);
		ModTotal mods[4];
		const int modCount = GetMods(mods, 4, 1000000);
		const bool workerOk = workerFound && onWorker.count > 0 && std::strcmp(onWorker.entries[0].name, "badmod.dll") == 0 &&
		                      onWorker.entries[0].share > 0.8f && modCount > 0 && std::strcmp(mods[0].name, "badmod.dll") == 0 &&
		                      mods[0].ownCores > 0.7f && mods[0].ownCores < 1.2f;
		WaitForSingleObject(worker, INFINITE);

		// A mod that only wraps a wait: the wait is not the mod's own code.
		BadModSleepFor(700);
		Snapshot wrapped;
		GetSnapshot(wrapped, 500000);
		const bool wrapOk = wrapped.waitShare > 0.8f && wrapped.modCount > 0 && std::strcmp(wrapped.mods[0].name, "badmod.dll") == 0 &&
		                    wrapped.mods[0].waiting > 0.8f && wrapped.mods[0].ownCode < 0.1f;

		char status[240];
		Status(status, sizeof(status));
		std::printf("profile checks: shares %d, range %d, describe %d, worker %d (found %d), wrap %d; %s\n", sharesOk, rangeOk, describeOk, workerOk,
			workerFound, wrapOk, status);
		skydiag::session::NoteCleanExit();
		return (all.samples >= 40 && sharesOk && rangeOk && describeOk && workerOk && wrapOk && all.costShare < 0.2f) ? 40 : 41;
	}
	if (std::wcscmp(mode, L"report") == 0) {
		using namespace skydiag::report;
		// The sessions of a character are in a folder with its name. The
		// old sessions here are for the character "Test: hero".
		const std::filesystem::path base = std::filesystem::path(argv[2]) / "Sessions";
		const std::filesystem::path dir = base / "Test_ hero";
		std::filesystem::create_directories(dir);
		// A session file from an older tool version, in the base folder.
		std::ofstream(base / "session-20250101-000000.sum") << "old";

		// 12 old sessions: 6 of 15 minutes with 60 FPS, 6 short ones.
		for (int i = 0; i < 12; ++i) {
			Summary old;
			old.durationSeconds = i % 2 ? 900 : 200;
			old.first.seconds = i % 2 ? 560 : 150;
			old.first.averageFps = 60;
			old.first.lowFps = 50;
			old.first.slowPerMinute = 1;
			char name[64];
			std::snprintf(name, sizeof(name), "session-20260101-0000%02d", i);
			std::ofstream(dir / (std::string(name) + ".sum")) << SummaryText(old);
			std::ofstream(dir / (std::string(name) + ".html")) << "old";
		}

		Begin(base, { "test", "Skyrim <test>", "test" });
		// No configuration file: the report uses the default suggestions.
		SetConfigFile(std::filesystem::path(argv[2]) / "no-such-file.ini");
		const bool moved = std::filesystem::exists(base / "No character" / "session-20250101-000000.sum");
		SetGroup("Test: hero");
		NoteProblem("fps-cpu");
		NoteProblem("fps-cpu");
		AddModLoad("Heavy<mod>.dll", 0.4f, 0.1f, 0.6f);
		AddScriptLoad("BusyScript", 2.0f, 5.0f);
		AddThreadWork("Thread that shows the frames", "d3d11.dll", 0.3f);
		EndLoadPeriod();
		const auto parsedSuggestions = ParseSuggestions("# c\n[a]\none\n two \n[b]\n\n[c]\nthree\n");
		const bool suggestionsOk = parsedSuggestions.size() == 3 && parsedSuggestions[0].second.size() == 2 && parsedSuggestions[0].second[1] == "two" &&
		                           parsedSuggestions[1].second.empty() && parsedSuggestions[2].first == "c" && MemoryText(900) == "900 MB" &&
		                           MemoryText(1536) == "1536 MB (1.5 GB)";
		int afterPrune = 0;
		for (const auto& entry : std::filesystem::directory_iterator(dir)) {
			(void)entry;
			++afterPrune;
		}

		// A session of 700 seconds at 48 FPS with a slow frame every 20 s
		// and one loading screen.
		bool        compared = false;
		int         comparedAt = 0;
		float       change = 0.0f;
		std::string text;
		for (int i = 0; i < 700; ++i) {
			Sample sample;
			sample.fps = 48;
			sample.worstFrameMs = (i % 20 == 0) ? 90.0f : 22.0f;
			sample.gameMb = 2000 + static_cast<std::uint32_t>(i);
			sample.vramMb = 3000;
			sample.loading = i >= 100 && i < 110;
			sample.vramBudgetMb = 3200;
			sample.gameMaxMb = 60000;
			AddSample(sample);
			if (i == 105) {
				SetCell("Test <cell> (interior)");
				AddEvent("Loading screen: 9.8 s");
			}
			if (i == 300) {
				AddEvent("Warning: 48 FPS <test> & more");
			}
			std::string now;
			float       nowChange = 0.0f;
			if (CheckComparison(now, nowChange)) {
				compared = true;
				comparedAt = i;
				change = nowChange;
				text = now;
			}
		}
		const bool written = Write();

		// 10 old sessions stay (20 files), plus the 2 files of this session.
		int total = 0;
		for (const auto& entry : std::filesystem::directory_iterator(dir)) {
			(void)entry;
			++total;
		}
		std::string html;
		for (const auto& entry : std::filesystem::directory_iterator(dir)) {
			if (entry.path().extension() == ".html" && entry.path().stem().string().rfind("session-20260101", 0) != 0) {
				std::ifstream     file(entry.path());
				std::stringstream content;
				content << file.rdbuf();
				html = content.str();
			}
		}
		// 5 of the 10 kept old sessions are long enough. 48 FPS against 60
		// and low FPS 48 against 50: (-20 % - 4 %) / 2 = -12 %.
		const bool ok = moved && suggestionsOk && afterPrune == 20 && written && total == 22 &&
		                html.find("Character: Test: hero") != std::string::npos && html.find("[fps-cpu]</h3>") != std::string::npos &&
		                html.find("SSE Engine Fixes") != std::string::npos && html.find("Heavy&lt;mod&gt;.dll</td><td></td><td>40.0 %") != std::string::npos &&
		                html.find("BusyScript</td><td></td><td>2.00") != std::string::npos && html.find("d3d11.dll</td><td>30.0 %") != std::string::npos && compared && comparedAt == 599 && change < -0.115f && change > -0.125f &&
		                text.find("12% worse") != std::string::npos && text.find("last 5 sessions") != std::string::npos &&
		                html.find("<svg") != std::string::npos && html.find("Skyrim &lt;test&gt;") != std::string::npos &&
		                html.find("48 FPS &lt;test&gt; &amp; more") != std::string::npos && html.find("Average of 5 earlier sessions") != std::string::npos &&
		                html.find("class=\"load\"") != std::string::npos && html.find("class=\"warn\"") != std::string::npos &&
		                html.find("class=\"loadspan\"") != std::string::npos && html.find("<th>No.</th>") != std::string::npos &&
		                html.find("<td class=\"loadnum\">1</td>") != std::string::npos && html.find("Draw calls in each frame") != std::string::npos &&
		                html.find("Test &lt;cell&gt; (interior)") != std::string::npos && html.find("maximum 3200 MB (3.1 GB)") != std::string::npos &&
		                html.find("Game memory (maximum 60000 MB (58.6 GB))") != std::string::npos && html.find("frame cap, measured: about 48 FPS") != std::string::npos;
		std::printf("report: prune %d, written %d, files %d, compared %d at %d, change %.3f, html %zu bytes: %s\n", afterPrune, written, total, compared,
			comparedAt, static_cast<double>(change), html.size(), ok ? "ok" : "wrong");
		skydiag::session::NoteCleanExit();
		return ok ? 70 : 71;
	}
	if (std::wcscmp(mode, L"settings") == 0) {
		const auto defaults = skydiag::ParseSettings("");
		const auto parsed = skydiag::ParseSettings(
			"; comment\n[Overlay]\nView = warnings ; text\nToggleKey = 0x7A\nWarningHoldSeconds = 12.5\n"
			"[Warnings]\nFpsBelow=45\n[Samplers]\nProfiler = 0\nPapyrus=1\nStallSeconds = 90\nUnknownKey = 3\nToggleKey = bad\n"
			"Animations = 0\nMemoryByDll = 0\nOverlayTarget = swapchain\nPapyrusTime = 0\n");
		const bool ok = defaults.view == 0 && defaults.toggleKey == 0x79 && defaults.profiler && defaults.papyrus && defaults.stallSeconds == 30 &&
		                parsed.view == 1 && parsed.toggleKey == 0x7A && parsed.warningHoldSeconds == 12.5f && parsed.fpsBelow == 45.0f &&
		                !parsed.profiler && parsed.papyrus && parsed.stallSeconds == 90 && defaults.animations && !parsed.animations && defaults.memoryByDll && !parsed.memoryByDll && defaults.overlayOnGameTarget &&
		                !parsed.overlayOnGameTarget && defaults.papyrusTime && !parsed.papyrusTime;
		std::printf("settings %s\n", ok ? "ok" : "wrong");
		skydiag::session::NoteCleanExit();
		return ok ? 60 : 61;
	}
	if (std::wcscmp(mode, L"advisor") == 0) {
		using namespace skydiag::advisor;
		Warning out[8];
		bool    ok = true;
		const auto has = [&](int a_count, const char* a_id, const char* a_text) {
			for (int i = 0; i < a_count; ++i) {
				if (std::strcmp(out[i].id, a_id) == 0 && std::strstr(out[i].text, a_text) != nullptr) {
					return true;
				}
			}
			return false;
		};

		// A good state: no warnings.
		Inputs good;
		good.frames.frames = 300;
		good.frames.fps = 60;
		good.frames.averageMs = 16.7f;
		good.frames.worstMs = 17.5f;
		good.frames.latencyMs = 17;
		good.frames.limit = skydiag::frames::Limit::kLimiter;
		good.gameMb = 4000;
		good.freeMb = 30000;
		good.vramUsedMb = 3000;
		good.vramBudgetMb = 8000;
		ok = ok && Evaluate(good, out, 8) == 0;

		// 35 FPS. The thread that shows the frames waits for the game update
		// thread, where a mod DLL works.
		Inputs slow = good;
		slow.frames.fps = 35;
		slow.frames.averageMs = 28.5f;
		slow.frames.worstMs = 33;
		slow.frames.latencyMs = 29;
		slow.frames.limit = skydiag::frames::Limit::kCpu;
		slow.threadCount = 2;
		slow.threads[0].isRender = true;
		slow.threads[0].waitShare = 0.7f;
		slow.threads[1].isTask = true;
		slow.threads[1].waitShare = 0.05f;
		slow.modCount = 1;
		std::snprintf(slow.mods[0].name, sizeof(slow.mods[0].name), "HeavyMod.dll");
		slow.mods[0].ownCores = 0.4f;
		slow.mods[0].totalCores = 0.5f;
		int n = Evaluate(slow, out, 8);
		ok = ok && EffectiveLimit(slow) == skydiag::frames::Limit::kCpu && has(n, "fps-cpu", "the CPU") &&
		     has(n, "fps-cpu", "HeavyMod.dll uses 40% of one processor core");

		// The work of a mod DLL moves between threads, so each thread waits
		// most of its time, and the frame time is regular. The mod keeps
		// most of one core busy: CPU, and the mod is named.
		Inputs spread = slow;
		spread.frames.limit = skydiag::frames::Limit::kLimiter;
		spread.frames.presentShare = 0.0f;
		spread.threads[1].waitShare = 0.8f;
		spread.mods[0].ownCores = 0.78f;
		spread.mods[0].totalCores = 0.78f;
		n = Evaluate(spread, out, 8);
		ok = ok && EffectiveLimit(spread) == skydiag::frames::Limit::kCpu && has(n, "fps-cpu", "HeavyMod.dll uses 78%");

		// A regular frame time, but the thread that shows the frames has no
		// wait: CPU, in the graphics layer.
		Inputs busy = good;
		busy.frames.fps = 45;
		busy.frames.averageMs = 22;
		busy.frames.limit = skydiag::frames::Limit::kLimiter;
		busy.frames.presentShare = 0.02f;
		busy.threadCount = 1;
		busy.threads[0].isRender = true;
		busy.threads[0].waitShare = 0.04f;
		std::snprintf(busy.threads[0].topModule, sizeof(busy.threads[0].topModule), "d3d11.dll");
		busy.threads[0].topModuleShare = 0.6f;
		n = Evaluate(busy, out, 8);
		ok = ok && EffectiveLimit(busy) == skydiag::frames::Limit::kCpu && has(n, "fps-cpu", "d3d11.dll uses 60% of the thread that shows the frames");

		// A busy thread that is not on the frame path does not make a CPU limit.
		Inputs background = good;
		background.frames.fps = 40;
		background.frames.averageMs = 25;
		background.frames.limit = skydiag::frames::Limit::kLimiter;
		background.threadCount = 2;
		background.threads[0].isRender = true;
		background.threads[0].waitShare = 0.6f;
		background.threads[1].waitShare = 0.0f;
		n = Evaluate(background, out, 8);
		ok = ok && EffectiveLimit(background) == skydiag::frames::Limit::kLimiter && has(n, "fps-cap", "frame cap");

		// Low FPS where all threads wait, most of it inside a mod: a frame
		// cap, and the mod is named.
		Inputs capped = slow;
		capped.threads[1].waitShare = 0.5f;
		capped.mods[0].ownCores = 0.0f;
		capped.mods[0].waitingCores = 0.45f;
		capped.mods[0].totalCores = 0.47f;
		std::snprintf(capped.mods[0].name, sizeof(capped.mods[0].name), "Limiter.dll");
		n = Evaluate(capped, out, 8);
		ok = ok && EffectiveLimit(capped) == skydiag::frames::Limit::kLimiter && has(n, "fps-cap", "frame cap") && has(n, "fps-cap", "Limiter.dll") &&
		     !has(n, "fps-cpu", "the CPU");

		// During and just after a loading screen: no frame or Papyrus warnings.
		Inputs loadingNow = slow;
		loadingNow.settled = false;
		loadingNow.hasPapyrus = true;
		loadingNow.papyrusMaxStacks = 1800;
		ok = ok && Evaluate(loadingNow, out, 8) == 0;

		// 35 FPS, GPU limit, with a queue and thus high latency.
		Inputs gpu = good;
		gpu.frames.fps = 35;
		gpu.frames.averageMs = 28.5f;
		gpu.frames.gpuQueue = 1.5f;
		gpu.frames.latencyMs = 71;
		gpu.frames.limit = skydiag::frames::Limit::kGpu;
		n = Evaluate(gpu, out, 8);
		ok = ok && has(n, "fps-gpu", "the GPU") && has(n, "latency", "queue");

		// Memory almost full, video memory full, memory growth, many stacks.
		Inputs bad = good;
		bad.freeMb = 600;
		bad.vramUsedMb = 8200;
		bad.hasTrend = true;
		bad.trendMbPerMinute = 60;
		bad.trendRecentMbPerMinute = 60;
		bad.trendMinutes = 6;
		bad.hasPapyrus = true;
		bad.papyrusMaxStacks = 900;
		std::snprintf(bad.papyrusTopScript, sizeof(bad.papyrusTopScript), "BusyScript");
		n = Evaluate(bad, out, 8);
		ok = ok && n == 4 && out[0].severity == Severity::kCritical && has(n, "memory-full", "crash is probable") && has(n, "vram-full", "8200 MB (8.0 GB) of 8000 MB (7.8 GB)") &&
		     has(n, "memory-growth", "60 MB each minute") && has(n, "papyrus-stacks", "BusyScript");

		// Memory that grew before but is stable in the last 2 minutes: no
		// growth warning.
		Inputs stable = good;
		stable.hasTrend = true;
		stable.trendMbPerMinute = 60;
		stable.trendRecentMbPerMinute = 1;
		stable.trendMinutes = 8;
		ok = ok && Evaluate(stable, out, 8) == 0;

		// Object references: 85 % in use is a warning, 96 % is critical.
		Inputs refs = good;
		refs.referenceLimit = 1048576;
		refs.references = 891290;
		n = Evaluate(refs, out, 8);
		ok = ok && n == 1 && out[0].severity == Severity::kWarning && has(n, "references", "891290 of 1048576");
		refs.references = 1006633;
		n = Evaluate(refs, out, 8);
		ok = ok && n == 1 && out[0].severity == Severity::kCritical && has(n, "references", "crash is probable");

		// The draw call count is in the cause text for the graphics layer.
		busy.drawCalls = 9000;
		n = Evaluate(busy, out, 8);
		ok = ok && has(n, "fps-cpu", "9000 draw calls");

		// Plugins: near the limit is a warning that shows only in the first
		// minute in the game. Over the limit is critical and shows always.
		Inputs plugins = good;
		plugins.fullPlugins = 240;
		n = Evaluate(plugins, out, 8);
		ok = ok && n == 1 && out[0].severity == Severity::kWarning && has(n, "plugins", "near the limit: 240 of 254");
		plugins.pluginWarningTime = false;
		ok = ok && Evaluate(plugins, out, 8) == 0;
		plugins.fullPlugins = 100;
		plugins.lightPlugins = 4100;
		n = Evaluate(plugins, out, 8);
		ok = ok && n == 1 && out[0].severity == Severity::kCritical && has(n, "plugins", "over the limit");

		std::printf("advisor %s\n", ok ? "ok" : "wrong");
		skydiag::session::NoteCleanExit();
		return ok ? 50 : 51;
	}
	if (std::wcscmp(mode, L"frame-stats") == 0) {
		using namespace skydiag::frames;
		// a_vary: frame time change from frame to frame, in percent.
		const auto feed = [](std::uint32_t a_frameUs, std::uint32_t a_presentUs, std::uint32_t a_queue, std::uint32_t a_cpuUs = 0,
							  std::uint32_t a_vary = 0) {
			for (std::uint32_t i = 0; i < 400; ++i) {
				const std::uint32_t change = a_frameUs * a_vary / 100 * (i % 5) / 4;
				Add({ a_frameUs + change, a_presentUs, a_queue, a_cpuUs });
			}
			return Compute(3'000'000);
		};
		bool ok = true;
		// 60 FPS with a long wait in Present and a GPU that keeps up: limiter.
		Summary s = feed(16667, 9000, 0);
		ok = ok && s.limit == Limit::kLimiter && s.fps > 59.0f && s.fps < 61.0f;
		// About 28 FPS with frame times that change, almost no wait in
		// Present, GPU keeps up: CPU.
		s = feed(33333, 500, 0, 0, 20);
		ok = ok && s.limit == Limit::kCpu && s.fps > 25.0f && s.fps < 31.0f;
		// A constant 60 FPS with a busy render thread and no wait in
		// Present (a cap that keeps the thread busy): cap.
		s = feed(16667, 200, 0, 16300);
		ok = ok && s.limit == Limit::kLimiter;
		// 60 FPS, no wait in Present, render thread idle most of the frame,
		// regular frame time: a frame cap.
		for (int i = 0; i < 400; ++i) {
			Add({ 16667, 300, 0, 6000 });
		}
		s = Compute(3'000'000);
		ok = ok && s.limit == Limit::kLimiter && s.latencyMs > 16.0f && s.latencyMs < 17.5f;
		// Frame times that change, render thread busy all the time: CPU.
		s = feed(30000, 300, 0, 31000, 20);
		ok = ok && s.limit == Limit::kCpu;
		// GPU one frame behind: GPU.
		s = feed(25000, 12000, 1);
		ok = ok && s.limit == Limit::kGpu;
		// One slow frame in 200: the 1 % low and the worst value must show it.
		for (int i = 0; i < 199; ++i) {
			Add({ 10000, 500, 0 });
		}
		Add({ 120000, 500, 0 });
		s = Compute(2'000'000);
		ok = ok && s.worstMs > 119.0f && s.low1Ms < 121.0f && s.averageMs < 11.0f;
		std::printf("frame stats %s\n", ok ? "ok" : "wrong");
		skydiag::session::NoteCleanExit();
		return ok ? 30 : 31;
	}
	if (std::wcscmp(mode, L"memory-scan") == 0) {
		// The scan must find the 64 MB that this test commits.
		const auto before = skydiag::session::PrivateMemoryByScanForTest();
		void*      block = VirtualAlloc(nullptr, 64 * 1024 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		const auto after = skydiag::session::PrivateMemoryByScanForTest();
		const auto grown = (after - before) / (1024 * 1024);
		std::printf("scan before %llu MB, after %llu MB\n", static_cast<unsigned long long>(before / (1024 * 1024)),
			static_cast<unsigned long long>(after / (1024 * 1024)));
		skydiag::session::NoteCleanExit();
		return (block && before > 0 && grown >= 64 && grown <= 80) ? 20 : 21;
	}
	if (std::wcscmp(mode, L"session-kill") == 0) {
		TerminateProcess(GetCurrentProcess(), 7);
	}
	if (std::wcscmp(mode, L"session-frozen-kill") == 0) {
		skydiag::session::SampleInput frozen;
		frozen.updateResponseMs = 45000;
		skydiag::session::Sample(frozen);
		TerminateProcess(GetCurrentProcess(), 7);
	}
	if (std::wcscmp(mode, L"mod-av") == 0) {
		BadModNullWrite();
	} else if (std::wcscmp(mode, L"mod-cpp") == 0) {
		BadModThrow();
	} else if (std::wcscmp(mode, L"mod-overflow") == 0) {
		return BadModOverflow();
	} else if (std::wcscmp(mode, L"mod-object") == 0) {
		BadModGameObject();
	} else if (std::wcscmp(mode, L"stall-spin") == 0) {
		StartStallWatcher();
		BadModSpin();
	} else if (std::wcscmp(mode, L"stall-block") == 0) {
		StartStallWatcher();
		BadModBlock();
	} else if (std::wcscmp(mode, L"game-av") == 0) {
		volatile int* target = reinterpret_cast<int*>(0x0000123456780000ull);
		*target = 1;
	} else if (std::wcscmp(mode, L"game-badptr") == 0) {
		// Not a canonical address: the processor does not report the value.
		volatile int* target = reinterpret_cast<int*>(0x1234567800000000ull);
		*target = 1;
	}

	std::fprintf(stderr, "The test did not crash\n");
	return 0;
}
