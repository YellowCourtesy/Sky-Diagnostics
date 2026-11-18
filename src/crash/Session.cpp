#include "Session.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <psapi.h>

#include <atomic>
#include <cstddef>
#include <cstring>

#include "../MemText.h"

namespace skydiag::session
{
	namespace
	{
		constexpr char          kMagic[8] = { 'S', 'K', 'Y', 'D', 'I', 'A', 'G', '1' };
		constexpr std::uint32_t kVersion = 3;
		constexpr int           kEventCount = 256;
		constexpr int           kSampleCount = 600;  // 10 minutes at one sample each second
		constexpr std::uint64_t kMegabyte = 1024ull * 1024ull;

		// Memory is "almost full" below these values.
		constexpr std::uint64_t kLowCommit = 768 * kMegabyte;
		constexpr std::uint64_t kLowPhysical = 384 * kMegabyte;

		enum State : std::uint32_t
		{
			kRunning = 1,
			kCleanExit = 2,
			kCrashReported = 3,
		};

		struct EventRecord
		{
			std::uint64_t time;  // FILETIME, UTC
			char          text[120];
		};

		struct SampleRecord
		{
			std::uint64_t time;
			std::uint64_t commit;            // private memory of the process
			std::uint64_t workingSet;        // physical memory of the process
			std::uint64_t systemCommitFree;  // memory that the process can still get
			std::uint64_t systemPhysicalFree;
			std::uint32_t handles;
			std::uint32_t updateSilentMs;
			std::uint32_t fpsTimes10;
			std::uint32_t worstFrameMs;
			std::uint32_t frameLimit;
			std::uint32_t reserved;
		};

		struct Record
		{
			char          magic[8];
			std::uint32_t version;
			std::uint32_t state;
			std::uint32_t processId;
			std::uint32_t stallCount;
			std::uint32_t commitByScan;  // 1 = private memory comes from an address space scan
			std::uint32_t reserved;
			std::uint64_t startTime;
			std::uint64_t lastAliveTime;
			std::uint64_t lastStallTime;
			char          toolVersion[32];
			char          gameVersion[64];
			char          platform[96];
			volatile LONG eventSeq;   // number of events written
			volatile LONG sampleSeq;  // number of samples written
			EventRecord   events[kEventCount];
			SampleRecord  samples[kSampleCount];
		};

		// Sum of the committed private memory regions of the process. Used
		// when the platform does not give the value (Wine gives zero).
		std::uint64_t PrivateCommitByScan()
		{
			SYSTEM_INFO info{};
			GetSystemInfo(&info);
			const auto end = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);

			std::uint64_t            total = 0;
			std::uintptr_t           address = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
			MEMORY_BASIC_INFORMATION region{};
			while (address < end && VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) == sizeof(region)) {
				if (region.RegionSize == 0) {
					break;
				}
				if (region.State == MEM_COMMIT && region.Type == MEM_PRIVATE) {
					total += region.RegionSize;
				}
				address = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
			}
			return total;
		}

		std::atomic<void (*)(const char*)> g_eventObserver{ nullptr };
		std::atomic<bool>      g_loading{ false };
		std::atomic<ULONGLONG> g_loadingEndTick{ 0 };  // 0 = no loading screen yet

		Record* g_record = nullptr;
		wchar_t g_reportDir[600];

		std::uint64_t NowFileTime()
		{
			FILETIME now;
			GetSystemTimeAsFileTime(&now);
			return (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
		}

		SYSTEMTIME ToLocal(std::uint64_t a_fileTime)
		{
			FILETIME utc;
			utc.dwLowDateTime = static_cast<DWORD>(a_fileTime);
			utc.dwHighDateTime = static_cast<DWORD>(a_fileTime >> 32);
			FILETIME   local{};
			SYSTEMTIME result{};
			if (FileTimeToLocalFileTime(&utc, &local)) {
				FileTimeToSystemTime(&local, &result);
			}
			return result;
		}

		void PutClock(Out& a_out, std::uint64_t a_fileTime)
		{
			const SYSTEMTIME t = ToLocal(a_fileTime);
			Put(a_out, "%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
		}

		unsigned long long Megabytes(std::uint64_t a_bytes)
		{
			return static_cast<unsigned long long>(a_bytes / kMegabyte);
		}

		// The newest sample, or nullptr.
		const SampleRecord* LastSample(const Record& a_record)
		{
			const LONG count = a_record.sampleSeq;
			if (count <= 0) {
				return nullptr;
			}
			return &a_record.samples[(count - 1) % kSampleCount];
		}

		void AppendEvents(Out& a_out, const Record& a_record, int a_max)
		{
			const LONG total = a_record.eventSeq;
			const LONG stored = total < kEventCount ? total : kEventCount;
			const LONG shown = stored < a_max ? stored : a_max;

			Put(a_out, "\n== LAST EVENTS (%ld of %ld) ==\n", shown, total);
			for (LONG i = total - shown; i < total; ++i) {
				const EventRecord& e = a_record.events[i % kEventCount];
				char               text[sizeof(e.text)];
				memcpy(text, e.text, sizeof(text));
				text[sizeof(text) - 1] = '\0';
				PutClock(a_out, e.time);
				Put(a_out, "  %s\n", text);
			}
		}

		void AppendMemory(Out& a_out, const Record& a_record)
		{
			const LONG total = a_record.sampleSeq;
			const LONG stored = total < kSampleCount ? total : kSampleCount;

			Put(a_out, "\n== MEMORY ==\n");
			if (stored == 0) {
				Put(a_out, "No samples.\n");
				return;
			}

			const SampleRecord& last = a_record.samples[(total - 1) % kSampleCount];
			const SampleRecord& first = a_record.samples[(total - stored) % kSampleCount];
			std::uint64_t       peak = 0;
			for (LONG i = total - stored; i < total; ++i) {
				const auto commit = a_record.samples[i % kSampleCount].commit;
				if (commit > peak) {
					peak = commit;
				}
			}

			Put(a_out, "Game memory (private): %s at the end, %s maximum in the last %ld s%s\n", MemText(Megabytes(last.commit)).text,
				MemText(Megabytes(peak)).text, stored, a_record.commitByScan ? " (measured by an address space scan one time each 5 s)" : "");
			Put(a_out, "Game memory (physical): %s\n", MemText(Megabytes(last.workingSet)).text);
			Put(a_out, "Memory that the game could still get: %s\n", MemText(Megabytes(last.systemCommitFree)).text);
			Put(a_out, "Free physical memory of the system: %s\n", MemText(Megabytes(last.systemPhysicalFree)).text);
			if (last.handles != 0) {
				Put(a_out, "Open handles: %lu\n", static_cast<unsigned long>(last.handles));
			} else {
				Put(a_out, "Open handles: not available on this platform\n");
			}

			const double seconds = static_cast<double>(last.time - first.time) / 10000000.0;
			if (seconds >= 30.0) {
				const double growth = (static_cast<double>(last.commit) - static_cast<double>(first.commit)) / static_cast<double>(kMegabyte);
				Put(a_out, "Change of game memory in the last %.0f s: %+.0f MB (%+.1f MB each minute)", seconds, growth, growth / (seconds / 60.0));
				if (growth >= 1024.0 || growth <= -1024.0) {
					Put(a_out, ", that is %+.1f GB", growth / 1024.0);
				}
				Put(a_out, "\n");
			}

			// One line for each 10 s of the last 2 minutes.
			Put(a_out, "Time      Game MB (GB)     Free MB (GB)     Update thread  FPS    Worst frame  Limit\n");
			const LONG window = stored < 120 ? stored : 120;
			for (LONG i = total - window; i < total; ++i) {
				if ((total - 1 - i) % 10 != 0) {
					continue;
				}
				const SampleRecord& s = a_record.samples[i % kSampleCount];
				PutClock(a_out, s.time);
				static const char* const limits[] = { "-", "CPU", "GPU", "limiter" };
				Put(a_out, "  %7llu (%5.1f)  %7llu (%5.1f)  %7lu ms  %5.1f  %8lu ms  %s\n", Megabytes(s.commit),
					static_cast<double>(Megabytes(s.commit)) / 1024.0, Megabytes(s.systemCommitFree),
					static_cast<double>(Megabytes(s.systemCommitFree)) / 1024.0, static_cast<unsigned long>(s.updateSilentMs), static_cast<double>(s.fpsTimes10) / 10.0, static_cast<unsigned long>(s.worstFrameMs),
					limits[s.frameLimit < 4 ? s.frameLimit : 0]);
			}
		}

		bool MemoryWarning(Out& a_out, const Record& a_record)
		{
			const SampleRecord* last = LastSample(a_record);
			if (!last) {
				return false;
			}
			if (last->systemCommitFree < kLowCommit || last->systemPhysicalFree < kLowPhysical) {
				Put(a_out, "Memory was almost full: the game could still get %s and the system had %s of free physical memory. "
						   "The game used %s.\n",
					MemText(Megabytes(last->systemCommitFree)).text, MemText(Megabytes(last->systemPhysicalFree)).text,
					MemText(Megabytes(last->commit)).text);
				return true;
			}
			return false;
		}

		bool ValidRecord(const Record& a_record)
		{
			return memcmp(a_record.magic, kMagic, sizeof(kMagic)) == 0 && a_record.version == kVersion;
		}

		// Makes the report for a previous session that stopped with no
		// crash report and no correct exit.
		// The meaning of a process exit code (an NTSTATUS value for most crashes).
		const char* ExitCodeText(DWORD a_code)
		{
			switch (a_code) {
			case 0:
				return "normal exit";
			case 1:
				return "stopped from outside (for example the task manager), or a program error";
			case 3:
				return "abort: a DLL stopped the program on purpose";
			case 0xC0000005:
				return "access violation";
			case 0xC00000FD:
				return "stack overflow";
			case 0xC0000374:
				return "heap corruption: code damaged the memory manager data";
			case 0xC0000409:
				return "fail fast: a DLL stopped the program at once (security check, __fastfail, or an error that it could not handle)";
			case 0xC0000017:
			case 0xC000012D:
				return "out of memory";
			case 0xC000001D:
				return "illegal instruction";
			case 0xC0000420:
				return "assertion failure";
			case 0xE06D7363:
				return "C++ exception that no code caught";
			case 0x40010004:
				return "the process was stopped by a debugger or from outside";
			default:
				return a_code >= 0xC0000000 ? "error (NTSTATUS)" : "not known";
			}
		}

		// a_exitCode: the exit code from the monitor, or kNoExitCode at the next game start.
		constexpr DWORD kNoExitCode = 0xFFFFFFFF;

		bool WriteUncleanReport(const Record& a_previous, DWORD a_exitCode = kNoExitCode, bool a_closing = false, wchar_t* a_pathOut = nullptr,
			std::size_t a_pathCap = 0)
		{
			static char buffer[256 * 1024];
			Out         out{ buffer, sizeof(buffer), 0 };
			buffer[0] = '\0';

			const SYSTEMTIME start = ToLocal(a_previous.startTime);
			const SYSTEMTIME end = ToLocal(a_previous.lastAliveTime);
			const auto       length = (a_previous.lastAliveTime - a_previous.startTime) / 10000000ull;
			const auto*      last = LastSample(a_previous);

			char tool[sizeof(a_previous.toolVersion) + 1] = {};
			char game[sizeof(a_previous.gameVersion) + 1] = {};
			char platform[sizeof(a_previous.platform) + 1] = {};
			memcpy(tool, a_previous.toolVersion, sizeof(a_previous.toolVersion));
			memcpy(game, a_previous.gameVersion, sizeof(a_previous.gameVersion));
			memcpy(platform, a_previous.platform, sizeof(a_previous.platform));

			Put(out, "SkyrimDiagnostics unclean exit report\n");
			if (a_exitCode == kNoExitCode) {
				Put(out, "The previous game session stopped without a correct exit and without a crash report.\n");
				Put(out, "This report was made at the next game start from the session record.\n\n");
			} else {
				Put(out, "The game stopped without a correct exit and without a crash report from the game.\n");
				Put(out, "The monitor process made this report when the game stopped.\n");
				Put(out, "Exit code: 0x%08lX (%s)\n", static_cast<unsigned long>(a_exitCode), ExitCodeText(a_exitCode));
				if (a_closing) {
					Put(out, "The player had chosen to quit or to close the window: this was during the game exit, not in play.\n");
				}
				Put(out, "\n");
			}
			Put(out, "Tool version: %s\n", tool);
			Put(out, "Game: %s\n", game);
			Put(out, "Platform: %s\n", platform);
			Put(out, "Session start: %04u-%02u-%02u %02u:%02u:%02u (local)\n", start.wYear, start.wMonth, start.wDay, start.wHour, start.wMinute,
				start.wSecond);
			Put(out, "Last sign of life: %04u-%02u-%02u %02u:%02u:%02u (local)\n", end.wYear, end.wMonth, end.wDay, end.wHour, end.wMinute,
				end.wSecond);
			Put(out, "Session length: %llu min %llu s\n", static_cast<unsigned long long>(length / 60), static_cast<unsigned long long>(length % 60));
			Put(out, "Stall reports in the session: %lu\n", static_cast<unsigned long>(a_previous.stallCount));

			AppendEvents(out, a_previous, 40);
			AppendMemory(out, a_previous);

			Put(out, "\n== PROBABLE CAUSE ==\n");
			const bool frozen = last && last->updateSilentMs >= 10000;
			if (a_exitCode == 0xC0000409 || a_exitCode == 0xC0000374 || a_exitCode == 0xC00000FD || a_exitCode == 3) {
				Put(out, "The game stopped with exit code 0x%08lX: %s.\n", static_cast<unsigned long>(a_exitCode), ExitCodeText(a_exitCode));
				Put(out, "In this type of stop the process ends at once, and no crash handler in the game can run. The DLL that did it cannot be named "
						 "from the exit code. Look at the last events above, and at the minidump if one was written.\n");
				Put(out, "Confidence: high for the type of stop, low for its source.\n");
			} else if (frozen) {
				Put(out, "The game update thread did not respond for %lu s when the process stopped.\n",
					static_cast<unsigned long>(last->updateSilentMs / 1000));
				Put(out, "The game was frozen (or in a very long load) and then something stopped the process, usually the user.\n");
				if (a_previous.stallCount > 0) {
					Put(out, "A stall report from this session shows what the game update thread did. Look for the newest stall-*.log before this report.\n");
				}
				Put(out, "Confidence: high that the game did not respond. The stall report names the source.\n");
				MemoryWarning(out, a_previous);
			} else if (MemoryWarning(out, a_previous)) {
				Put(out, "The process probably stopped because memory was full. The operating system can stop a process in this condition "
						 "without an exception (on Linux: the out-of-memory killer).\n");
				Put(out, "Confidence: medium.\n");
			} else {
				Put(out, "The record shows no freeze and no memory problem. The process stopped with no exception that the tool could see.\n");
				Put(out, "Possible causes: the process was stopped from outside (task manager, kill command, Steam), a fail-fast stop in a DLL, "
						 "a power loss, or a game exit that the tool did not detect.\n");
				Put(out, "Confidence: low. The last events above show what the game did before it stopped.\n");
			}

			wchar_t path[800];
			swprintf_s(path, L"%s\\unclean-exit-%04u-%02u-%02u-%02u-%02u-%02u.log", g_reportDir, end.wYear, end.wMonth, end.wDay, end.wHour,
				end.wMinute, end.wSecond);
			if (a_pathOut && a_pathCap > 0) {
				wcsncpy_s(a_pathOut, a_pathCap, path, _TRUNCATE);
			}
			const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return false;
			}
			// Head lines, then PROBABLE CAUSE (the end of the text), then the other sections.
			const char*       firstSection = strstr(out.buf, "\n== ");
			const std::size_t headLen = firstSection ? static_cast<std::size_t>(firstSection - out.buf) : out.len;
			const char*       cause = strstr(out.buf, "\n== PROBABLE CAUSE ==");
			const std::size_t causeAt = cause ? static_cast<std::size_t>(cause - out.buf) : out.len;
			DWORD             written = 0;
			bool              ok = WriteFile(file, out.buf, static_cast<DWORD>(headLen), &written, nullptr) != FALSE;
			if (causeAt < out.len) {
				ok = WriteFile(file, out.buf + causeAt, static_cast<DWORD>(out.len - causeAt), &written, nullptr) != FALSE && ok;
			}
			if (causeAt > headLen) {
				ok = WriteFile(file, out.buf + headLen, static_cast<DWORD>(causeAt - headLen), &written, nullptr) != FALSE && ok;
			}
			CloseHandle(file);
			return ok;
		}

		// Reads the record file of the previous session, if there is one.
		bool CheckPrevious(const wchar_t* a_path)
		{
			const HANDLE file = CreateFileW(a_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return false;
			}
			static Record previous;
			DWORD         read = 0;
			const bool    ok = ReadFile(file, &previous, sizeof(previous), &read, nullptr) && read == sizeof(previous);
			CloseHandle(file);

			if (!ok || !ValidRecord(previous) || previous.state != kRunning) {
				return false;
			}
			return WriteUncleanReport(previous);
		}
	}

	bool WriteExitReport(const wchar_t* a_dataDir, const wchar_t* a_reportDir, std::uint32_t a_exitCode, bool a_closing, wchar_t* a_pathOut,
		std::size_t a_pathCap)
	{
		if (!a_dataDir || !a_reportDir) {
			return false;
		}
		wcsncpy_s(g_reportDir, a_reportDir, _TRUNCATE);
		wchar_t path[800];
		swprintf_s(path, L"%s\\session.bin", a_dataDir);
		const HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			return false;
		}
		static Record previous;
		DWORD         read = 0;
		const bool    ok = ReadFile(file, &previous, sizeof(previous), &read, nullptr) && read == sizeof(previous);
		if (!ok || !ValidRecord(previous) || previous.state != kRunning) {
			CloseHandle(file);
			return false;
		}
		const bool written = WriteUncleanReport(previous, a_exitCode, a_closing, a_pathOut, a_pathCap);
		if (written) {
			// The next game start must not make a second report of this exit.
			const std::uint32_t state = kCrashReported;
			const LONG          offset = static_cast<LONG>(offsetof(Record, state));
			DWORD               done = 0;
			if (SetFilePointer(file, offset, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
				WriteFile(file, &state, sizeof(state), &done, nullptr);
			}
		}
		CloseHandle(file);
		return written;
	}

	bool Start(const Config& a_config, bool* a_previousUnclean)
	{
		if (a_previousUnclean) {
			*a_previousUnclean = false;
		}
		if (g_record || !a_config.dataDir || !a_config.reportDir) {
			return false;
		}
		wcsncpy_s(g_reportDir, a_config.reportDir, _TRUNCATE);

		wchar_t path[800];
		swprintf_s(path, L"%s\\session.bin", a_config.dataDir);

		const bool unclean = CheckPrevious(path);
		if (a_previousUnclean) {
			*a_previousUnclean = unclean;
		}

		const HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			return false;
		}
		const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(Record)), nullptr);
		if (!mapping) {
			CloseHandle(file);
			return false;
		}
		const auto record = static_cast<Record*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Record)));
		// The view keeps the mapping and the file open.
		CloseHandle(mapping);
		CloseHandle(file);
		if (!record) {
			return false;
		}

		memset(record, 0, sizeof(Record));
		memcpy(record->magic, kMagic, sizeof(kMagic));
		record->version = kVersion;
		record->processId = GetCurrentProcessId();
		record->startTime = NowFileTime();
		record->lastAliveTime = record->startTime;
		strncpy_s(record->toolVersion, a_config.toolVersion ? a_config.toolVersion : "", _TRUNCATE);
		strncpy_s(record->gameVersion, a_config.gameVersion ? a_config.gameVersion : "", _TRUNCATE);
		strncpy_s(record->platform, a_config.platform ? a_config.platform : "", _TRUNCATE);
		record->state = kRunning;

		g_record = record;
		return true;
	}

	void Event(const char* a_fmt, ...)
	{
		Record* record = g_record;
		if (!record) {
			return;
		}
		// The session record keeps the first part of a long text. The
		// observer (the session report) gets all of it.
		char    full[480];
		va_list args;
		va_start(args, a_fmt);
		_vsnprintf_s(full, sizeof(full), _TRUNCATE, a_fmt, args);
		va_end(args);
		char text[sizeof(EventRecord::text)];
		strncpy_s(text, full, _TRUNCATE);

		const LONG   index = InterlockedIncrement(&record->eventSeq) - 1;
		EventRecord& slot = record->events[index % kEventCount];
		slot.time = NowFileTime();
		memcpy(slot.text, text, sizeof(text));

		if (const auto observer = g_eventObserver.load()) {
			observer(full);
		}
	}

	std::uint64_t PrivateMemoryByScanForTest()
	{
		return PrivateCommitByScan();
	}

	bool GetLatest(Latest& a_out)
	{
		const Record* record = g_record;
		if (!record) {
			return false;
		}
		const SampleRecord* last = LastSample(*record);
		if (!last) {
			return false;
		}
		a_out.updateResponseMs = last->updateSilentMs;
		a_out.commit = last->commit;
		a_out.systemCommitFree = last->systemCommitFree;
		return true;
	}

	void SetEventObserver(void (*a_observer)(const char* a_text))
	{
		g_eventObserver.store(a_observer);
	}

	void SetLoading(bool a_loading)
	{
		if (!a_loading && g_loading.load()) {
			g_loadingEndTick = GetTickCount64();
		}
		g_loading = a_loading;
	}

	float SecondsSinceLoading()
	{
		if (g_loading.load()) {
			return 0.0f;
		}
		const ULONGLONG end = g_loadingEndTick.load();
		if (end == 0) {
			return 100000.0f;
		}
		return static_cast<float>(GetTickCount64() - end) / 1000.0f;
	}

	namespace
	{
		// Least squares line through (time, memory) of the samples
		// [a_first, a_last). Returns megabytes for each minute.
		bool Slope(const Record& a_record, LONG a_first, LONG a_last, double& a_slope)
		{
			const LONG n = a_last - a_first;
			if (n < 2) {
				return false;
			}
			const double t0 = static_cast<double>(a_record.samples[a_first % kSampleCount].time);
			double       sumT = 0, sumM = 0, sumTT = 0, sumTM = 0;
			for (LONG i = a_first; i < a_last; ++i) {
				const SampleRecord& sample = a_record.samples[i % kSampleCount];
				const double        t = (static_cast<double>(sample.time) - t0) / 600000000.0;  // minutes
				const double        m = static_cast<double>(sample.commit) / static_cast<double>(kMegabyte);
				sumT += t;
				sumM += m;
				sumTT += t * t;
				sumTM += t * m;
			}
			const double denominator = n * sumTT - sumT * sumT;
			if (denominator <= 0.0) {
				return false;
			}
			a_slope = (n * sumTM - sumT * sumM) / denominator;
			return true;
		}
	}

	bool GetMemoryTrend(float& a_mbPerMinute, float& a_minutes, float& a_recentMbPerMinute)
	{
		const Record* record = g_record;
		if (!record) {
			return false;
		}
		const LONG total = record->sampleSeq;
		const LONG stored = total < kSampleCount ? total : kSampleCount;

		// Go back from the newest sample to the last loading screen.
		LONG first = total;
		for (LONG i = total - 1; i >= total - stored; --i) {
			if (record->samples[i % kSampleCount].reserved & 1u) {
				break;
			}
			first = i;
		}
		if (total - first < 240) {
			return false;
		}

		double all = 0.0, recent = 0.0;
		if (!Slope(*record, first, total, all) || !Slope(*record, total - 120, total, recent)) {
			return false;
		}
		a_mbPerMinute = static_cast<float>(all);
		a_recentMbPerMinute = static_cast<float>(recent);
		a_minutes = static_cast<float>(
			(static_cast<double>(record->samples[(total - 1) % kSampleCount].time) - static_cast<double>(record->samples[first % kSampleCount].time)) /
			600000000.0);
		return true;
	}

	void Sample(const SampleInput& a_input)
	{
		Record* record = g_record;
		if (!record) {
			return;
		}
		SampleRecord sample{};
		sample.time = NowFileTime();
		sample.updateSilentMs = a_input.updateResponseMs;
		sample.fpsTimes10 = static_cast<std::uint32_t>(a_input.fps * 10.0f);
		sample.worstFrameMs = static_cast<std::uint32_t>(a_input.worstFrameMs);
		sample.frameLimit = a_input.frameLimit;
		sample.reserved = (a_input.loading || g_loading.load()) ? 1u : 0u;

		PROCESS_MEMORY_COUNTERS_EX counters{};
		counters.cb = sizeof(counters);
		if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
			sample.commit = counters.PrivateUsage;
			sample.workingSet = counters.WorkingSetSize;
		}
		if (sample.commit == 0 || record->commitByScan) {
			// The scan is slower than the counter: do it one time each 5 s.
			static std::uint64_t scanned = 0;
			static std::uint32_t age = 0;
			if (scanned == 0 || ++age >= 5) {
				scanned = PrivateCommitByScan();
				age = 0;
			}
			sample.commit = scanned;
			record->commitByScan = 1;
		}
		MEMORYSTATUSEX status{};
		status.dwLength = sizeof(status);
		if (GlobalMemoryStatusEx(&status)) {
			sample.systemCommitFree = status.ullAvailPageFile;
			sample.systemPhysicalFree = status.ullAvailPhys;
		}
		DWORD handles = 0;
		if (GetProcessHandleCount(GetCurrentProcess(), &handles)) {
			sample.handles = handles;
		}

		const LONG index = record->sampleSeq;
		record->samples[index % kSampleCount] = sample;
		InterlockedIncrement(&record->sampleSeq);
		record->lastAliveTime = sample.time;
	}

	void NoteCleanExit()
	{
		if (g_record && g_record->state == kRunning) {
			g_record->lastAliveTime = NowFileTime();
			g_record->state = kCleanExit;
		}
	}

	void NoteCrashReported()
	{
		if (g_record) {
			g_record->lastAliveTime = NowFileTime();
			g_record->state = kCrashReported;
		}
	}

	void NoteStall()
	{
		if (g_record) {
			++g_record->stallCount;
			g_record->lastStallTime = NowFileTime();
		}
	}

	void AppendRecent(Out& a_out)
	{
		if (!g_record) {
			return;
		}
		AppendEvents(a_out, *g_record, 30);
		AppendMemory(a_out, *g_record);
	}

	bool AppendMemoryWarning(Out& a_out)
	{
		return g_record && MemoryWarning(a_out, *g_record);
	}
}
