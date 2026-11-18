#include "Monitor.h"

#include "Session.h"
#include "Text.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <dbghelp.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wct.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>

// Rules for this file: the functions for the game (RequestDump, NoteReport,
// IsClosing) run after a crash. They must not allocate heap memory and must
// not take locks. Run is the monitor process: normal code.

namespace skydiag::monitor
{
	namespace
	{
		constexpr std::uint32_t kMagic = 0x4D474453;  // "SDGM"
		constexpr std::uint32_t kLayout = 1;

		// The shared memory between the game and the monitor.
		struct Shared
		{
			std::uint32_t   magic;
			std::uint32_t   layout;
			std::uint32_t   gamePid;
			std::uint32_t   monitorPid;
			volatile LONG64 beatTick;       // GetTickCount64 of the last game beat
			volatile LONG64 monitorTick;    // GetTickCount64 of the last monitor beat
			volatile LONG   mainThreadId;
			volatile LONG   loading;
			volatile LONG   quit;
			volatile LONG64 closeTick;      // 0 = no close request
			volatile LONG   dumpState;      // 0 none, 1 requested, 2 written, 3 failed
			std::uint32_t   dumpThreadId;
			std::uint64_t   dumpPointers;   // EXCEPTION_POINTERS* in the game, 0 = freeze
			wchar_t         dumpPath[600];
			volatile LONG   reportState;    // 0 none, 1 a crash report was written
			volatile LONG   openReport;     // open it after the game closed
			std::uint64_t   crashFileTime;
			wchar_t         reportPath[600];
			std::uint32_t   freezeSeconds;
			std::uint32_t   optOpenReport;
			std::uint32_t   optMinidump;
			wchar_t         reportDir[600];
			wchar_t         dataDir[600];
			wchar_t         skseDir[600];
			wchar_t         logPath[600];
			wchar_t         modsFile[600];
			wchar_t         pluginsDir[600];  // the folder of this DLL (Data/SKSE/Plugins)
		};

		Shared* g_shared = nullptr;
		HANDLE  g_dumpRequest = nullptr;  // event: the game asks for a dump
		HANDLE  g_dumpDone = nullptr;     // event: the monitor wrote it

		void Name(wchar_t* a_out, std::size_t a_cap, std::uint32_t a_pid, const wchar_t* a_what)
		{
			swprintf_s(a_out, a_cap, L"Local\\SkyrimDiagnostics-Monitor-%lu%s", static_cast<unsigned long>(a_pid), a_what);
		}

		// ------------------------------------------------------- monitor log

		HANDLE g_log = INVALID_HANDLE_VALUE;

		void Log(const char* a_fmt, ...)
		{
			if (g_log == INVALID_HANDLE_VALUE) {
				return;
			}
			char       line[1200];
			SYSTEMTIME now;
			GetLocalTime(&now);
			int n = _snprintf_s(line, _TRUNCATE, "[%02u:%02u:%02u.%03u] ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
			if (n < 0) {
				n = 0;
			}
			va_list args;
			va_start(args, a_fmt);
			const int m = _vsnprintf_s(line + n, sizeof(line) - n - 2, _TRUNCATE, a_fmt, args);
			va_end(args);
			n += m < 0 ? static_cast<int>(strlen(line + n)) : m;
			line[n++] = '\r';
			line[n++] = '\n';
			DWORD written = 0;
			WriteFile(g_log, line, static_cast<DWORD>(n), &written, nullptr);
		}

		void Utf8(const wchar_t* a_in, char* a_out, int a_cap)
		{
			if (WideCharToMultiByte(CP_UTF8, 0, a_in, -1, a_out, a_cap, nullptr, nullptr) == 0) {
				a_out[0] = '\0';
			}
		}

		// ------------------------------------------------------------- dumps

		bool WriteDump(HANDLE a_process, DWORD a_pid, const wchar_t* a_path, DWORD a_threadId, std::uint64_t a_pointers)
		{
			const HANDLE file = CreateFileW(a_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return false;
			}
			MINIDUMP_EXCEPTION_INFORMATION info{};
			info.ThreadId = a_threadId;
			info.ExceptionPointers = reinterpret_cast<PEXCEPTION_POINTERS>(a_pointers);
			info.ClientPointers = TRUE;  // the pointers are in the game process
			const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
														 MiniDumpWithHandleData | MiniDumpWithIndirectlyReferencedMemory);
			const bool ok = MiniDumpWriteDump(a_process, a_pid, file, type, a_pointers ? &info : nullptr, nullptr, nullptr) != FALSE;
			CloseHandle(file);
			if (!ok) {
				DeleteFileW(a_path);
			}
			return ok;
		}

		// ----------------------------------------------- freeze from outside

		const char* WaitTypeText(WCT_OBJECT_TYPE a_type)
		{
			switch (a_type) {
			case WctCriticalSectionType:
				return "a lock (critical section)";
			case WctSendMessageType:
				return "a window message to a different thread";
			case WctMutexType:
				return "a mutex";
			case WctAlpcType:
				return "a call to a system service (ALPC)";
			case WctComType:
				return "a COM call";
			case WctThreadWaitType:
				return "a thread to end";
			case WctProcessWaitType:
				return "a process to end";
			case WctThreadType:
				return "thread";
			case WctComActivationType:
				return "a COM activation";
			case WctSocketIoType:
				return "network input or output";
			case WctSmbIoType:
				return "file input or output on a network share";
			default:
				return "something not known";
			}
		}

		// The wait chain of a thread (Windows Wait Chain Traversal): what it
		// waits for, and which thread holds it.
		void WaitChain(Out& a_out, DWORD a_threadId, DWORD* a_owner)
		{
			if (a_owner) {
				*a_owner = 0;
			}
			const HWCT session = OpenThreadWaitChainSession(0, nullptr);
			if (!session) {
				Put(a_out, "The wait chain is not available (error %lu).\n", GetLastError());
				return;
			}
			WAITCHAIN_NODE_INFO nodes[WCT_MAX_NODE_COUNT];
			DWORD               count = WCT_MAX_NODE_COUNT;
			BOOL                cycle = FALSE;
			if (!GetThreadWaitChain(session, 0, WCTP_GETINFO_ALL_FLAGS, a_threadId, &count, nodes, &cycle)) {
				Put(a_out, "The wait chain is not available (error %lu).\n", GetLastError());
				CloseThreadWaitChainSession(session);
				return;
			}
			for (DWORD i = 0; i < count; ++i) {
				const auto& n = nodes[i];
				if (n.ObjectType == WctThreadType) {
					if (a_owner && *a_owner == 0 && n.ThreadObject.ThreadId != a_threadId) {
						*a_owner = n.ThreadObject.ThreadId;
					}
					Put(a_out, "  Thread %lu (process %lu)%s\n", n.ThreadObject.ThreadId, n.ThreadObject.ProcessId,
						n.ObjectStatus == WctStatusBlocked ? " is blocked and waits for:" : "");
				} else {
					char name[300];
					Utf8(n.LockObject.ObjectName, name, sizeof(name));
					Put(a_out, "    %s%s%s%s\n", WaitTypeText(n.ObjectType), name[0] ? " \"" : "", name, name[0] ? "\"" : "");
				}
			}
			if (cycle) {
				Put(a_out, "  The chain is a cycle: the threads wait for each other (a deadlock).\n");
			}
			CloseThreadWaitChainSession(session);
		}

		// Visible dialog windows of the game process (an error message that a DLL opened).
		struct DialogSearch
		{
			DWORD pid;
			Out*  out;
			int   count;
		};

		BOOL CALLBACK FindDialog(HWND a_window, LPARAM a_param)
		{
			auto* search = reinterpret_cast<DialogSearch*>(a_param);
			DWORD pid = 0;
			GetWindowThreadProcessId(a_window, &pid);
			if (pid != search->pid || !IsWindowVisible(a_window)) {
				return TRUE;
			}
			wchar_t className[64];
			if (GetClassNameW(a_window, className, static_cast<int>(std::size(className))) == 0 || wcscmp(className, L"#32770") != 0) {
				return TRUE;
			}
			// InternalGetWindowText sends no message: a frozen window thread cannot stop this.
			wchar_t title[256] = L"";
			InternalGetWindowText(a_window, title, static_cast<int>(std::size(title)));
			wchar_t text[1024] = L"";
			if (const HWND child = FindWindowExW(a_window, nullptr, L"Static", nullptr)) {
				// The message text is usually the second static control (the first is the icon).
				const HWND second = FindWindowExW(a_window, child, L"Static", nullptr);
				InternalGetWindowText(second ? second : child, text, static_cast<int>(std::size(text)));
			}
			char t8[512], x8[2048];
			Utf8(title, t8, sizeof(t8));
			Utf8(text, x8, sizeof(x8));
			Put(*search->out, "  A dialog window is open: \"%s\": %s\n", t8, x8);
			++search->count;
			return TRUE;
		}

		int Dialogs(Out& a_out, DWORD a_pid)
		{
			DialogSearch search{ a_pid, &a_out, 0 };
			EnumWindows(&FindDialog, reinterpret_cast<LPARAM>(&search));
			return search.count;
		}

		void WriteText(const wchar_t* a_path, const Out& a_out)
		{
			const HANDLE file = CreateFileW(a_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return;
			}
			DWORD written = 0;
			WriteFile(file, a_out.buf, static_cast<DWORD>(a_out.len), &written, nullptr);
			CloseHandle(file);
		}

		void TimeName(wchar_t* a_out, std::size_t a_cap, const wchar_t* a_dir, const wchar_t* a_kind, const wchar_t* a_ext)
		{
			SYSTEMTIME now;
			GetLocalTime(&now);
			swprintf_s(a_out, a_cap, L"%s\\%s-%04u-%02u-%02u-%02u-%02u-%02u%s", a_dir, a_kind, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
				now.wSecond, a_ext);
		}

		// All of the game stopped (also the tool's watchdog thread): the
		// freeze report from outside.
		void WriteOutsideFreezeReport(HANDLE a_process, DWORD a_pid, ULONGLONG a_silentSeconds)
		{
			static char buffer[256 * 1024];
			Out         out{ buffer, sizeof(buffer), 0 };
			buffer[0] = '\0';
			Put(out, "SkyrimDiagnostics freeze report (from the monitor process)\n");
			Put(out, "No part of the game process answered for %llu s, also not the watchdog thread of this tool.\n", a_silentSeconds);
			Put(out, "Thus the stall report in the game could not be made. This report comes from outside the game.\n");

			Put(out, "\n== WAIT CHAIN OF THE GAME UPDATE THREAD ==\n");
			const DWORD mainThread = static_cast<DWORD>(g_shared->mainThreadId);
			if (mainThread) {
				WaitChain(out, mainThread, nullptr);
			} else {
				Put(out, "The game update thread is not known yet.\n");
			}

			Put(out, "\n== WAIT CHAINS OF ALL BLOCKED THREADS ==\n");
			const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snapshot != INVALID_HANDLE_VALUE) {
				THREADENTRY32 entry{};
				entry.dwSize = sizeof(entry);
				int shown = 0;
				if (Thread32First(snapshot, &entry)) {
					do {
						if (entry.th32OwnerProcessID == a_pid && entry.th32ThreadID != mainThread && shown < 32) {
							const std::size_t mark = out.len;
							WaitChain(out, entry.th32ThreadID, nullptr);
							// Keep only chains with more than the thread itself.
							if (out.len - mark < 60) {
								out.len = mark;
								buffer[mark] = '\0';
							} else {
								++shown;
							}
						}
						entry.dwSize = sizeof(entry);
					} while (Thread32Next(snapshot, &entry));
				}
				CloseHandle(snapshot);
			}

			Put(out, "\n== DIALOG WINDOWS ==\n");
			const int dialogs = Dialogs(out, a_pid);
			if (dialogs == 0) {
				Put(out, "None.\n");
			}

			Put(out, "\n== PROBABLE CAUSE ==\n");
			if (dialogs > 0) {
				Put(out, "A dialog window of the game is open. A DLL showed an error message and the game waits for it. Read the message above: it "
						 "usually names the DLL or the problem (for example an Address Library error: a DLL does not support this game version).\n");
				Put(out, "Confidence: high.\n");
			} else {
				Put(out, "All threads of the game stopped. The usual cause is a deadlock: threads that wait for each other (see the wait chains "
						 "above), often while a DLL loads or unloads.\n");
				Put(out, "Confidence: low. Open the minidump in WinDbg for the call stacks.\n");
			}

			wchar_t path[700];
			TimeName(path, std::size(path), g_shared->reportDir, L"freeze-outside", L".log");
			if (g_shared->optMinidump) {
				wchar_t dump[700];
				TimeName(dump, std::size(dump), g_shared->reportDir, L"freeze-outside", L".dmp");
				const bool ok = WriteDump(a_process, a_pid, dump, mainThread, 0);
				Put(out, "\nMinidump: %s\n", ok ? "written (same file name, .dmp)" : "not written (error)");
			}
			WriteText(path, out);
			char p8[800];
			Utf8(path, p8, sizeof(p8));
			Log("Freeze report from outside written: %s", p8);
		}

		// ------------------------------------------------- Crash Logger log

		std::uint64_t FileTime(const FILETIME& a_time)
		{
			return (static_cast<std::uint64_t>(a_time.dwHighDateTime) << 32) | a_time.dwLowDateTime;
		}

		// Adds the Crash Logger log of the same crash (made at most 2 minutes
		// after the crash) to the end of the crash report.
		void AddCrashLoggerLog(const wchar_t* a_report, std::uint64_t a_crashTime)
		{
			const wchar_t* folders[] = { L"", L"\\Crashlogs" };
			wchar_t        best[800] = L"";
			std::uint64_t  bestTime = 0;
			for (const wchar_t* sub : folders) {
				wchar_t pattern[800];
				swprintf_s(pattern, L"%s%s\\crash-*.log", g_shared->skseDir, sub);
				WIN32_FIND_DATAW data{};
				const HANDLE     find = FindFirstFileW(pattern, &data);
				if (find == INVALID_HANDLE_VALUE) {
					continue;
				}
				do {
					const std::uint64_t written = FileTime(data.ftLastWriteTime);
					constexpr std::uint64_t kTwoMinutes = 2ull * 60 * 10000000;
					if (written + 10000000ull >= a_crashTime && written <= a_crashTime + kTwoMinutes && written > bestTime) {
						bestTime = written;
						swprintf_s(best, L"%s%s\\%s", g_shared->skseDir, sub, data.cFileName);
					}
				} while (FindNextFileW(find, &data));
				FindClose(find);
			}
			if (best[0] == L'\0') {
				Log("No Crash Logger log of this crash was found.");
				return;
			}
			const HANDLE in = CreateFileW(best, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (in == INVALID_HANDLE_VALUE) {
				return;
			}
			static char text[2 * 1024 * 1024];
			DWORD       read = 0;
			ReadFile(in, text, sizeof(text) - 1, &read, nullptr);
			CloseHandle(in);
			text[read] = '\0';

			const HANDLE out = CreateFileW(a_report, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (out == INVALID_HANDLE_VALUE) {
				return;
			}
			char name[800];
			Utf8(best, name, sizeof(name));
			char  head[1000];
			const int n = _snprintf_s(head, _TRUNCATE,
				"\n== CRASH LOGGER LOG ==\nThe log of Crash Logger for the same crash: %s\nCompare its PROBABLE CALL STACK and POSSIBLE RELEVANT OBJECTS with the "
				"sections above.\n\n",
				name);
			DWORD written = 0;
			WriteFile(out, head, static_cast<DWORD>(n > 0 ? n : 0), &written, nullptr);
			WriteFile(out, text, read, &written, nullptr);
			CloseHandle(out);
			Log("Crash Logger log added to the crash report: %s", name);
		}

		// ---------------------------------------------- PDB function names

		// For each call stack frame in a mod DLL that has a PDB file next to
		// it (in the folder of the SKSE plugins): the function name and the
		// source line. Added to the end of the crash report.
		void AddPdbNames(const wchar_t* a_report)
		{
			static char text[2 * 1024 * 1024];
			const HANDLE in = CreateFileW(a_report, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (in == INVALID_HANDLE_VALUE) {
				return;
			}
			DWORD read = 0;
			ReadFile(in, text, sizeof(text) - 1, &read, nullptr);
			CloseHandle(in);
			text[read] = '\0';
			const char* begin = strstr(text, "\n== CALL STACK ==\n");
			if (!begin) {
				return;
			}
			begin += 18;
			const char* end = strstr(begin, "\n== ");
			if (!end) {
				end = text + read;
			}

			const HANDLE self = GetCurrentProcess();
			SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
			if (!SymInitializeW(self, g_shared->pluginsDir, FALSE)) {
				return;
			}
			struct Loaded
			{
				char      name[96];
				DWORD64   base;  // 0 = no PDB
			};
			static Loaded loaded[64];
			int           loadedCount = 0;

			static char section[256 * 1024];
			Out         out{ section, sizeof(section), 0 };
			section[0] = '\0';
			int found = 0;
			for (const char* line = begin; line < end;) {
				const char* next = static_cast<const char*>(memchr(line, '\n', static_cast<std::size_t>(end - line)));
				int                frame = -1;
				unsigned long long address = 0;
				char               where[160] = "";
				if (sscanf_s(line, "#%d 0x%llX %159s", &frame, &address, where, static_cast<unsigned>(sizeof(where))) == 3) {
					char* plus = strrchr(where, '+');
					if (plus && plus - where > 4 && _strnicmp(plus - 4, ".dll", 4) == 0) {
						*plus = '\0';
						const unsigned long long offset = std::strtoull(plus + 1, nullptr, 16);
						int k = 0;
						for (; k < loadedCount && _stricmp(loaded[k].name, where) != 0; ++k) {
						}
						if (k == loadedCount && loadedCount < 64) {
							Loaded& l = loaded[loadedCount++];
							strncpy_s(l.name, where, _TRUNCATE);
							l.base = 0;
							wchar_t wide[96];
							MultiByteToWideChar(CP_UTF8, 0, where, -1, wide, static_cast<int>(std::size(wide)));
							wchar_t dll[800], pdb[800];
							swprintf_s(dll, L"%s\\%s", g_shared->pluginsDir, wide);
							wcsncpy_s(pdb, dll, _TRUNCATE);
							if (wchar_t* dot = wcsrchr(pdb, L'.')) {
								wcscpy_s(dot, 5, L".pdb");
							}
							if (GetFileAttributesW(dll) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(pdb) != INVALID_FILE_ATTRIBUTES) {
								const DWORD64 base = 0x100000000ull * static_cast<DWORD64>(loadedCount);
								l.base = SymLoadModuleExW(self, nullptr, dll, nullptr, base, 0, nullptr, 0);
							}
						}
						if (k < loadedCount && loaded[k].base != 0) {
							alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
							auto*                     symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
							symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
							symbol->MaxNameLen = 511;
							DWORD64 displacement = 0;
							if (SymFromAddr(self, loaded[k].base + offset, &displacement, symbol)) {
								Put(out, "#%-3d %s+%llX  %s+0x%llX", frame, where, offset, symbol->Name, static_cast<unsigned long long>(displacement));
								IMAGEHLP_LINE64 lineInfo{};
								lineInfo.SizeOfStruct = sizeof(lineInfo);
								DWORD lineDisplacement = 0;
								if (SymGetLineFromAddr64(self, loaded[k].base + offset, &lineDisplacement, &lineInfo)) {
									const char* file = strrchr(lineInfo.FileName, '\\');
									Put(out, "  (%s line %lu)", file ? file + 1 : lineInfo.FileName, lineInfo.LineNumber);
								}
								Put(out, "\n");
								++found;
							}
						}
					}
				}
				line = next ? next + 1 : end;
			}
			SymCleanup(self);
			if (found == 0) {
				return;
			}
			const HANDLE file = CreateFileW(a_report, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return;
			}
			const char head[] = "\n== FUNCTION NAMES (from the PDB files of mod DLLs) ==\n";
			DWORD      written = 0;
			WriteFile(file, head, static_cast<DWORD>(sizeof(head) - 1), &written, nullptr);
			WriteFile(file, out.buf, static_cast<DWORD>(out.len), &written, nullptr);
			CloseHandle(file);
			Log("Function names from PDB files added to the crash report: %d frames.", found);
		}

		// ------------------------------------------------------- after exit

		void AfterExit(HANDLE a_process)
		{
			DWORD code = 0;
			GetExitCodeProcess(a_process, &code);
			const bool closing = g_shared->quit != 0 || g_shared->closeTick != 0;
			Log("The game process ended with exit code 0x%08lX%s.", code, closing ? " after a quit or close request" : "");

			if (g_shared->reportState == 1) {
				AddPdbNames(g_shared->reportPath);
				AddCrashLoggerLog(g_shared->reportPath, g_shared->crashFileTime);
				// The MO2 mod list of this game start, next to the report (the next start writes a new one).
				if (g_shared->modsFile[0] && GetFileAttributesW(g_shared->modsFile) != INVALID_FILE_ATTRIBUTES) {
					wchar_t copy[800];
					wcsncpy_s(copy, g_shared->reportPath, _TRUNCATE);
					if (wchar_t* dot = wcsrchr(copy, L'.')) {
						*dot = L'\0';
					}
					wcsncat_s(copy, L"-mods.tsv", _TRUNCATE);
					if (CopyFileW(g_shared->modsFile, copy, FALSE)) {
						Log("MO2 mod list copied next to the crash report.");
					}
				}
				if (g_shared->optOpenReport && g_shared->openReport) {
					const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", g_shared->reportPath, nullptr, nullptr, SW_SHOWNORMAL));
					Log("Crash report opened (result %lld).", static_cast<long long>(result));
				}
				return;
			}
			if (code == 0) {
				return;
			}
			wchar_t path[800] = L"";
			if (session::WriteExitReport(g_shared->dataDir, g_shared->reportDir, code, closing, path, std::size(path))) {
				char p8[800];
				Utf8(path, p8, sizeof(p8));
				Log("Unclean exit report written: %s", p8);
				// Crash Logger can take a crash before this tool: then its log is the
				// only crash data. Add it to the report.
				FILETIME nowTime{};
				GetSystemTimeAsFileTime(&nowTime);
				AddCrashLoggerLog(path, FileTime(nowTime) - 2ull * 60 * 10000000);
				if (g_shared->optOpenReport && !closing) {
					ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);
				}
			}
		}
	}

	void AppendWaitChain(Out& a_out, std::uint32_t a_threadId, std::uint32_t* a_owner)
	{
		DWORD owner = 0;
		WaitChain(a_out, a_threadId, &owner);
		if (a_owner) {
			*a_owner = owner;
		}
	}

	int AppendDialogs(Out& a_out, std::uint32_t a_processId)
	{
		return Dialogs(a_out, a_processId);
	}

	// ================================================================ game

	bool Start(const Options& a_options)
	{
		if (g_shared || !a_options.reportDir || !a_options.dataDir) {
			return false;
		}
		// This DLL: the file of the module that has this function.
		wchar_t dllPath[MAX_PATH * 2] = L"";
		if (a_options.dllPath) {
			wcsncpy_s(dllPath, a_options.dllPath, _TRUNCATE);
		} else {
			HMODULE self = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Start),
					&self) ||
				GetModuleFileNameW(self, dllPath, static_cast<DWORD>(std::size(dllPath))) == 0) {
				return false;
			}
		}
		const DWORD pid = GetCurrentProcessId();
		wchar_t     name[128];
		Name(name, std::size(name), pid, L"");
		const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(Shared)), name);
		if (!mapping) {
			return false;
		}
		auto* shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
		if (!shared) {
			CloseHandle(mapping);
			return false;
		}
		// The game keeps the mapping open until it ends.
		memset(shared, 0, sizeof(Shared));
		shared->magic = kMagic;
		shared->layout = kLayout;
		shared->gamePid = pid;
		shared->beatTick = static_cast<LONG64>(GetTickCount64());
		shared->freezeSeconds = a_options.freezeSeconds;
		shared->optOpenReport = a_options.openReport ? 1 : 0;
		shared->optMinidump = a_options.writeMinidump ? 1 : 0;
		wcsncpy_s(shared->reportDir, a_options.reportDir, _TRUNCATE);
		wcsncpy_s(shared->dataDir, a_options.dataDir, _TRUNCATE);
		wcsncpy_s(shared->skseDir, a_options.skseDir ? a_options.skseDir : L"", _TRUNCATE);
		wcsncpy_s(shared->logPath, a_options.logPath ? a_options.logPath : L"", _TRUNCATE);
		wcsncpy_s(shared->modsFile, a_options.modsFile ? a_options.modsFile : L"", _TRUNCATE);
		wcsncpy_s(shared->pluginsDir, dllPath, _TRUNCATE);
		if (wchar_t* slash = wcsrchr(shared->pluginsDir, L'\\')) {
			*slash = L'\0';
		}

		wchar_t eventName[160];
		Name(eventName, std::size(eventName), pid, L"-Dump");
		g_dumpRequest = CreateEventW(nullptr, FALSE, FALSE, eventName);
		Name(eventName, std::size(eventName), pid, L"-DumpDone");
		g_dumpDone = CreateEventW(nullptr, FALSE, FALSE, eventName);
		g_shared = shared;

		// rundll32.exe "<this DLL>",SkyDiagMonitor <pid>
		wchar_t system[MAX_PATH];
		if (GetSystemDirectoryW(system, MAX_PATH) == 0) {
			return false;
		}
		wchar_t program[MAX_PATH + 32];
		swprintf_s(program, L"%s\\rundll32.exe", system);
		wchar_t command[1400];
		swprintf_s(command, L"\"%s\" \"%s\",SkyDiagMonitor %lu", program, dllPath, static_cast<unsigned long>(pid));
		STARTUPINFOW        startup{};
		PROCESS_INFORMATION process{};
		startup.cb = sizeof(startup);
		if (!CreateProcessW(program, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &startup, &process)) {
			return false;
		}
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		return true;
	}

	bool IsRunning()
	{
		return g_shared && g_shared->monitorPid != 0 && GetTickCount64() - static_cast<ULONGLONG>(g_shared->monitorTick) < 5000;
	}

	void Beat()
	{
		if (g_shared) {
			g_shared->beatTick = static_cast<LONG64>(GetTickCount64());
		}
	}

	void SetMainThread(std::uint32_t a_threadId)
	{
		if (g_shared) {
			g_shared->mainThreadId = static_cast<LONG>(a_threadId);
		}
	}

	void SetLoading(bool a_loading)
	{
		if (g_shared) {
			g_shared->loading = a_loading ? 1 : 0;
		}
	}

	void NoteQuit()
	{
		if (g_shared) {
			g_shared->quit = 1;
		}
	}

	void NoteCloseRequest()
	{
		if (g_shared) {
			g_shared->closeTick = static_cast<LONG64>(GetTickCount64());
		}
	}

	namespace
	{
		WNDPROC g_oldWindowProc = nullptr;

		LRESULT CALLBACK WindowProc(HWND a_window, UINT a_message, WPARAM a_wparam, LPARAM a_lparam)
		{
			if (a_message == WM_CLOSE || (a_message == WM_SYSCOMMAND && (a_wparam & 0xFFF0) == SC_CLOSE)) {
				if (!g_shared || g_shared->closeTick == 0) {
					session::Event("Game window: close requested (Alt+F4, the X button or a close message)");
				}
				NoteCloseRequest();
			}
			return CallWindowProcW(g_oldWindowProc, a_window, a_message, a_wparam, a_lparam);
		}

		BOOL CALLBACK FindGameWindow(HWND a_window, LPARAM a_param)
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(a_window, &pid);
			if (pid == GetCurrentProcessId() && IsWindowVisible(a_window) && GetWindow(a_window, GW_OWNER) == nullptr) {
				*reinterpret_cast<HWND*>(a_param) = a_window;
				return FALSE;
			}
			return TRUE;
		}
	}

	bool WatchWindowClose()
	{
		if (g_oldWindowProc) {
			return true;
		}
		HWND window = nullptr;
		EnumWindows(&FindGameWindow, reinterpret_cast<LPARAM>(&window));
		if (!window) {
			return false;
		}
		g_oldWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc)));
		return g_oldWindowProc != nullptr;
	}

	bool IsClosing()
	{
		if (!g_shared) {
			return false;
		}
		const auto close = static_cast<ULONGLONG>(g_shared->closeTick);
		return g_shared->quit != 0 || (close != 0 && GetTickCount64() - close < 10000);
	}

	bool RequestDump(const wchar_t* a_path, const void* a_pointers, std::uint32_t a_threadId, std::uint32_t a_timeoutMs)
	{
		if (!IsRunning() || !g_dumpRequest || !g_dumpDone || !a_path) {
			return false;
		}
		wcsncpy_s(g_shared->dumpPath, a_path, _TRUNCATE);
		g_shared->dumpThreadId = a_threadId;
		g_shared->dumpPointers = reinterpret_cast<std::uint64_t>(a_pointers);
		g_shared->dumpState = 1;
		ResetEvent(g_dumpDone);
		SetEvent(g_dumpRequest);
		if (WaitForSingleObject(g_dumpDone, a_timeoutMs) != WAIT_OBJECT_0) {
			return false;
		}
		return g_shared->dumpState == 2;
	}

	void NoteReport(const wchar_t* a_path, bool a_open, std::uint64_t a_crashFileTime)
	{
		if (!g_shared || !a_path) {
			return;
		}
		wcsncpy_s(g_shared->reportPath, a_path, _TRUNCATE);
		g_shared->crashFileTime = a_crashFileTime;
		g_shared->openReport = a_open ? 1 : 0;
		g_shared->reportState = 1;
	}

	// ============================================================= monitor

	void Run(const char* a_commandLine)
	{
		const DWORD pid = static_cast<DWORD>(std::strtoul(a_commandLine ? a_commandLine : "", nullptr, 10));
		if (pid == 0) {
			return;
		}
		wchar_t name[128];
		Name(name, std::size(name), pid, L"");
		const HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
		if (!mapping) {
			return;
		}
		g_shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
		if (!g_shared || g_shared->magic != kMagic || g_shared->layout != kLayout) {
			return;
		}
		if (g_shared->logPath[0]) {
			g_log = CreateFileW(g_shared->logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		}
		const HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
		if (!process) {
			Log("The game process %lu could not be opened (error %lu). The monitor stops.", pid, GetLastError());
			return;
		}
		wchar_t eventName[160];
		Name(eventName, std::size(eventName), pid, L"-Dump");
		const HANDLE request = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, eventName);
		Name(eventName, std::size(eventName), pid, L"-DumpDone");
		const HANDLE done = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, eventName);

		g_shared->monitorPid = GetCurrentProcessId();
		g_shared->monitorTick = static_cast<LONG64>(GetTickCount64());
		Log("Monitor started for the game process %lu (freeze limit %lu s, open report %lu, minidump %lu).", pid,
			static_cast<unsigned long>(g_shared->freezeSeconds), static_cast<unsigned long>(g_shared->optOpenReport),
			static_cast<unsigned long>(g_shared->optMinidump));

		bool outsideFreezeReported = false;
		for (;;) {
			const HANDLE handles[2] = { process, request };
			const DWORD  result = WaitForMultipleObjects(request ? 2 : 1, handles, FALSE, 1000);
			g_shared->monitorTick = static_cast<LONG64>(GetTickCount64());
			if (result == WAIT_OBJECT_0) {
				break;
			}
			if (result == WAIT_OBJECT_0 + 1) {
				const bool ok = WriteDump(process, pid, g_shared->dumpPath, g_shared->dumpThreadId, g_shared->dumpPointers);
				g_shared->dumpState = ok ? 2 : 3;
				char p8[800];
				Utf8(g_shared->dumpPath, p8, sizeof(p8));
				Log("Minidump %s: %s", ok ? "written" : "not written", p8);
				if (done) {
					SetEvent(done);
				}
				continue;
			}

			// All of the game stopped: the tool's watchdog thread beats each
			// second. A freeze that the game reports itself does not stop it.
			const ULONGLONG silent = (GetTickCount64() - static_cast<ULONGLONG>(g_shared->beatTick)) / 1000;
			const ULONGLONG limit = static_cast<ULONGLONG>(g_shared->freezeSeconds) + 15;
			if (!outsideFreezeReported && silent > limit && g_shared->quit == 0 && g_shared->closeTick == 0) {
				outsideFreezeReported = true;
				Log("The game process did not answer for %llu s.", silent);
				WriteOutsideFreezeReport(process, pid, silent);
			} else if (outsideFreezeReported && silent < 5) {
				outsideFreezeReported = false;
				Log("The game process answers again.");
			}
		}
		AfterExit(process);
		CloseHandle(process);
		Log("Monitor stopped.");
		if (g_log != INVALID_HANDLE_VALUE) {
			CloseHandle(g_log);
		}
	}
}
