#include "LoadTimes.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <mutex>

namespace skydiag::loadtimes
{
	namespace
	{
		// ------------------------------------------------------------ timing

		double Ms(LONGLONG a_ticks)
		{
			static const LONGLONG frequency = [] {
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				return f.QuadPart;
			}();
			return static_cast<double>(a_ticks) * 1000.0 / static_cast<double>(frequency);
		}

		LONGLONG Now()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		std::mutex       g_lock;
		std::vector<Row> g_rows;

		void Record(const char* a_module, const char* a_what, double a_ms)
		{
			const std::scoped_lock lock{ g_lock };
			for (auto& row : g_rows) {
				if (row.module == a_module && row.what == a_what) {
					row.totalMs += a_ms;
					row.maxMs = (std::max)(row.maxMs, a_ms);
					++row.count;
					return;
				}
			}
			g_rows.push_back(Row{ a_module, a_what, a_ms, a_ms, 1 });
		}

		void ModuleOf(const void* a_address, char* a_out, std::size_t a_cap)
		{
			strncpy_s(a_out, a_cap, "(not in a module)", _TRUNCATE);
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(a_address),
					&module)) {
				return;
			}
			wchar_t path[MAX_PATH * 2];
			if (GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path))) == 0) {
				return;
			}
			const wchar_t* name = wcsrchr(path, L'\\');
			WideCharToMultiByte(CP_UTF8, 0, name ? name + 1 : path, -1, a_out, static_cast<int>(a_cap), nullptr, nullptr);
		}

		// ------------------------------------------------------------- stubs
		//
		// A stub calls a wrapper with the original function: it puts a pointer
		// to its data into RDX (the second argument) and jumps to the wrapper.
		// The functions that get a stub have one argument (RCX), so RDX is free.

		struct Meta
		{
			void* original;
			char  module[96];
			int   kind;  // 0 plugin load, 1 message listener, 2 co-save load, 3 co-save revert
		};

		constexpr int  kMaxStubs = 4096;
		Meta           g_meta[kMaxStubs];
		unsigned char* g_stubs = nullptr;
		LONG           g_stubCount = 0;

		void* MakeStub(void* a_wrapper, void* a_original, const void* a_moduleAddress, int a_kind)
		{
			if (!g_stubs) {
				g_stubs = static_cast<unsigned char*>(VirtualAlloc(nullptr, kMaxStubs * 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
				if (!g_stubs) {
					return nullptr;
				}
			}
			const LONG index = InterlockedIncrement(&g_stubCount) - 1;
			if (index >= kMaxStubs) {
				return nullptr;
			}
			Meta& meta = g_meta[index];
			meta.original = a_original;
			meta.kind = a_kind;
			ModuleOf(a_moduleAddress, meta.module, sizeof(meta.module));

			unsigned char* code = g_stubs + index * 32;
			const auto     metaAddress = reinterpret_cast<std::uint64_t>(&meta);
			const auto     wrapperAddress = reinterpret_cast<std::uint64_t>(a_wrapper);
			code[0] = 0x48;  // mov rdx, imm64
			code[1] = 0xBA;
			std::memcpy(code + 2, &metaAddress, 8);
			code[10] = 0x48;  // mov rax, imm64
			code[11] = 0xB8;
			std::memcpy(code + 12, &wrapperAddress, 8);
			code[20] = 0xFF;  // jmp rax
			code[21] = 0xE0;
			FlushInstructionCache(GetCurrentProcess(), code, 32);
			return code;
		}

		// ---------------------------------------------------------- wrappers

		bool PluginLoadWrapper(void* a_interface, Meta* a_meta)
		{
			const LONGLONG start = Now();
			const bool     result = reinterpret_cast<bool (*)(void*)>(a_meta->original)(a_interface);
			Record(a_meta->module, "SKSEPlugin_Load", Ms(Now() - start));
			return result;
		}

		void ListenerWrapper(void* a_message, Meta* a_meta)
		{
			const std::uint32_t type = a_message ? *static_cast<const std::uint32_t*>(a_message) : 0xFFFFFFFF;
			const LONGLONG      start = Now();
			reinterpret_cast<void (*)(void*)>(a_meta->original)(a_message);
			char what[64];
			_snprintf_s(what, _TRUNCATE, "message %s", MessageName(type));
			Record(a_meta->module, what, Ms(Now() - start));
		}

		void SerializationWrapper(void* a_interface, Meta* a_meta)
		{
			const LONGLONG start = Now();
			reinterpret_cast<void (*)(void*)>(a_meta->original)(a_interface);
			Record(a_meta->module, a_meta->kind == 2 ? "co-save load" : "co-save revert", Ms(Now() - start));
		}

		// --------------------------------------------- SKSE GetProcAddress

		using GetProcAddress_t = FARPROC(WINAPI*)(HMODULE, LPCSTR);
		GetProcAddress_t g_getProcAddress = nullptr;

		FARPROC WINAPI HookGetProcAddress(HMODULE a_module, LPCSTR a_name)
		{
			FARPROC result = g_getProcAddress(a_module, a_name);
			if (result && reinterpret_cast<std::uintptr_t>(a_name) > 0xFFFF && std::strcmp(a_name, "SKSEPlugin_Load") == 0) {
				if (void* stub = MakeStub(reinterpret_cast<void*>(&PluginLoadWrapper), reinterpret_cast<void*>(result), reinterpret_cast<const void*>(result), 0)) {
					return reinterpret_cast<FARPROC>(stub);
				}
			}
			return result;
		}

		// Replaces the import a_function of a_dll in the import table of a_module.
		void* PatchImport(HMODULE a_module, const char* a_dll, const char* a_function, void* a_new)
		{
			auto*       base = reinterpret_cast<unsigned char*>(a_module);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
				return nullptr;
			}
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (dir.VirtualAddress == 0) {
				return nullptr;
			}
			for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
				if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), a_dll) != 0 || imp->OriginalFirstThunk == 0) {
					continue;
				}
				auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk);
				auto* thunks = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
				for (; names->u1.AddressOfData; ++names, ++thunks) {
					if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) {
						continue;
					}
					const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
					if (std::strcmp(reinterpret_cast<const char*>(byName->Name), a_function) != 0) {
						continue;
					}
					DWORD old = 0;
					if (!VirtualProtect(&thunks->u1.Function, sizeof(thunks->u1.Function), PAGE_READWRITE, &old)) {
						return nullptr;
					}
					void* original = reinterpret_cast<void*>(thunks->u1.Function);
					thunks->u1.Function = reinterpret_cast<ULONGLONG>(a_new);
					VirtualProtect(&thunks->u1.Function, sizeof(thunks->u1.Function), old, &old);
					return original;
				}
			}
			return nullptr;
		}

		// ------------------------------------------------- SKSE interfaces

		using RegisterListener_t = bool (*)(std::uint32_t, const char*, void*);
		using SetCallback_t = void (*)(std::uint32_t, void*);
		RegisterListener_t g_registerListener = nullptr;
		SetCallback_t      g_setLoadCallback = nullptr;
		SetCallback_t      g_setRevertCallback = nullptr;

		bool HookRegisterListener(std::uint32_t a_listener, const char* a_sender, void* a_handler)
		{
			void* handler = a_handler;
			if (a_handler) {
				if (void* stub = MakeStub(reinterpret_cast<void*>(&ListenerWrapper), a_handler, a_handler, 1)) {
					handler = stub;
				}
			}
			return g_registerListener(a_listener, a_sender, handler);
		}

		void HookSetLoadCallback(std::uint32_t a_plugin, void* a_callback)
		{
			void* callback = a_callback;
			if (a_callback) {
				if (void* stub = MakeStub(reinterpret_cast<void*>(&SerializationWrapper), a_callback, a_callback, 2)) {
					callback = stub;
				}
			}
			g_setLoadCallback(a_plugin, callback);
		}

		void HookSetRevertCallback(std::uint32_t a_plugin, void* a_callback)
		{
			void* callback = a_callback;
			if (a_callback) {
				if (void* stub = MakeStub(reinterpret_cast<void*>(&SerializationWrapper), a_callback, a_callback, 3)) {
					callback = stub;
				}
			}
			g_setRevertCallback(a_plugin, callback);
		}

		// Replaces a function pointer in an SKSE interface structure.
		template <class T>
		T PatchPointer(const void* a_interface, std::size_t a_offset, T a_new)
		{
			auto* slot = reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(a_interface) + a_offset);
			DWORD old = 0;
			if (!VirtualProtect(slot, sizeof(T), PAGE_READWRITE, &old)) {
				return nullptr;
			}
			const T original = *slot;
			*slot = a_new;
			VirtualProtect(slot, sizeof(T), old, &old);
			return original;
		}

		// ------------------------------------------------------ plugin files

		struct FileOpen
		{
			char     name[128];
			LONGLONG time;
		};
		constexpr int kMaxFiles = 8192;
		FileOpen      g_files[kMaxFiles];
		int           g_fileCount = 0;
		std::mutex    g_fileLock;
		LONGLONG      g_dataLoadedTime = 0;

		using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
		using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
		CreateFileA_t g_createFileA = nullptr;
		CreateFileW_t g_createFileW = nullptr;

		bool IsPluginFile(const char* a_name)
		{
			const std::size_t n = std::strlen(a_name);
			return n > 4 && (_stricmp(a_name + n - 4, ".esp") == 0 || _stricmp(a_name + n - 4, ".esm") == 0 || _stricmp(a_name + n - 4, ".esl") == 0);
		}

		void NoteOpen(const char* a_path)
		{
			if (g_dataLoadedTime != 0) {
				return;  // only the data load of the game start
			}
			const char* name = std::strrchr(a_path, '\\');
			const char* slash = std::strrchr(a_path, '/');
			name = (std::max)(name, slash);
			name = name ? name + 1 : a_path;
			if (!IsPluginFile(name)) {
				return;
			}
			const std::scoped_lock lock{ g_fileLock };
			// The last open of each file counts: the first opens only read the
			// file headers (the load order).
			for (int i = 0; i < g_fileCount; ++i) {
				if (_stricmp(g_files[i].name, name) == 0) {
					g_files[i].time = Now();
					return;
				}
			}
			if (g_fileCount < kMaxFiles) {
				strncpy_s(g_files[g_fileCount].name, name, _TRUNCATE);
				g_files[g_fileCount].time = Now();
				++g_fileCount;
			}
		}

		// ---------------------------------------------- slow file reads

		// The file name of each handle that the game opened (a small table by
		// handle value; an old entry can be replaced, then the name is "?").
		struct HandleName
		{
			HANDLE handle;
			char   name[96];
		};
		HandleName g_handleNames[4096];

		const char* NameOf(HANDLE a_handle)
		{
			const auto& entry = g_handleNames[(reinterpret_cast<std::uintptr_t>(a_handle) >> 2) % std::size(g_handleNames)];
			return entry.handle == a_handle ? entry.name : "?";
		}

		void NoteHandle(HANDLE a_handle, const char* a_path)
		{
			if (a_handle == INVALID_HANDLE_VALUE || !a_handle) {
				return;
			}
			const char* name = std::strrchr(a_path, '\\');
			const char* slash = std::strrchr(a_path, '/');
			name = (std::max)(name, slash);
			name = name ? name + 1 : a_path;
			auto& entry = g_handleNames[(reinterpret_cast<std::uintptr_t>(a_handle) >> 2) % std::size(g_handleNames)];
			entry.handle = a_handle;
			strncpy_s(entry.name, name, _TRUNCATE);
		}

		struct SlowRead
		{
			LONGLONG end;
			float    ms;
			char     name[96];
			bool     open;
		};
		constexpr int kSlowRing = 256;
		SlowRead      g_slow[kSlowRing];
		volatile LONG g_slowCount = 0;

		void NoteSlow(const char* a_name, LONGLONG a_start, LONGLONG a_end, bool a_open)
		{
			const double ms = Ms(a_end - a_start);
			if (ms < 8.0) {
				return;
			}
			const LONG index = InterlockedIncrement(&g_slowCount) - 1;
			SlowRead&  entry = g_slow[index % kSlowRing];
			entry.end = a_end;
			entry.ms = static_cast<float>(ms);
			entry.open = a_open;
			strncpy_s(entry.name, a_name, _TRUNCATE);
			Record(a_name, a_open ? "slow file open" : "slow file read", ms);
		}

		using ReadFile_t = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
		ReadFile_t g_readFile = nullptr;

		BOOL WINAPI HookReadFile(HANDLE a_file, LPVOID a_buffer, DWORD a_size, LPDWORD a_read, LPOVERLAPPED a_overlapped)
		{
			if (a_overlapped) {
				return g_readFile(a_file, a_buffer, a_size, a_read, a_overlapped);  // asynchronous: the time is not the read
			}
			const LONGLONG start = Now();
			const BOOL     result = g_readFile(a_file, a_buffer, a_size, a_read, a_overlapped);
			NoteSlow(NameOf(a_file), start, Now(), false);
			return result;
		}

		HANDLE WINAPI HookCreateFileA(LPCSTR a_path, DWORD a_access, DWORD a_share, LPSECURITY_ATTRIBUTES a_security, DWORD a_disposition, DWORD a_flags,
			HANDLE a_template)
		{
			if (a_path) {
				NoteOpen(a_path);
			}
			const LONGLONG start = Now();
			const HANDLE   result = g_createFileA(a_path, a_access, a_share, a_security, a_disposition, a_flags, a_template);
			if (a_path) {
				NoteHandle(result, a_path);
				NoteSlow(NameOf(result), start, Now(), true);
			}
			return result;
		}

		HANDLE WINAPI HookCreateFileW(LPCWSTR a_path, DWORD a_access, DWORD a_share, LPSECURITY_ATTRIBUTES a_security, DWORD a_disposition, DWORD a_flags,
			HANDLE a_template)
		{
			char path[600] = "";
			if (a_path && WideCharToMultiByte(CP_UTF8, 0, a_path, -1, path, sizeof(path), nullptr, nullptr) != 0) {
				NoteOpen(path);
			}
			const LONGLONG start = Now();
			const HANDLE   result = g_createFileW(a_path, a_access, a_share, a_security, a_disposition, a_flags, a_template);
			if (path[0]) {
				NoteHandle(result, path);
				NoteSlow(NameOf(result), start, Now(), true);
			}
			return result;
		}
	}

	const char* MessageName(std::uint32_t a_type)
	{
		static const char* const kNames[] = { "PostLoad", "PostPostLoad", "PreLoadGame", "PostLoadGame", "SaveGame", "DeleteGame", "InputLoaded", "NewGame",
			"DataLoaded" };
		return a_type < std::size(kNames) ? kNames[a_type] : "other";
	}

	void HookPluginLoad()
	{
		if (g_getProcAddress) {
			return;
		}
		// The SKSE DLL: skse64_*.dll or sksevr_*.dll.
		HMODULE modules[1024];
		DWORD   bytes = 0;
		if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) {
			return;
		}
		for (DWORD i = 0; i < bytes / sizeof(HMODULE); ++i) {
			wchar_t name[MAX_PATH];
			if (K32GetModuleBaseNameW(GetCurrentProcess(), modules[i], name, MAX_PATH) == 0) {
				continue;
			}
			if (_wcsnicmp(name, L"skse64_", 7) != 0 && _wcsnicmp(name, L"sksevr_", 7) != 0) {
				continue;
			}
			if (auto* original = PatchImport(modules[i], "KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(&HookGetProcAddress))) {
				g_getProcAddress = reinterpret_cast<GetProcAddress_t>(original);
			}
			return;
		}
	}

	void HookFileOpen()
	{
		if (g_createFileA || g_createFileW) {
			return;
		}
		const HMODULE game = GetModuleHandleW(nullptr);
		if (auto* a = PatchImport(game, "KERNEL32.dll", "CreateFileA", reinterpret_cast<void*>(&HookCreateFileA))) {
			g_createFileA = reinterpret_cast<CreateFileA_t>(a);
		}
		if (auto* w = PatchImport(game, "KERNEL32.dll", "CreateFileW", reinterpret_cast<void*>(&HookCreateFileW))) {
			g_createFileW = reinterpret_cast<CreateFileW_t>(w);
		}
		if (auto* r = PatchImport(game, "KERNEL32.dll", "ReadFile", reinterpret_cast<void*>(&HookReadFile))) {
			g_readFile = reinterpret_cast<ReadFile_t>(r);
		}
	}

	void HookInterfaces(const void* a_messaging, const void* a_serialization)
	{
		// SKSE interface layouts (PluginAPI.h of SKSE, the same for SE, AE and VR):
		// SKSEMessagingInterface: version (4 bytes and padding), RegisterListener at 8.
		// SKSESerializationInterface: version, SetUniqueID at 8, SetRevertCallback at 16,
		// SetSaveCallback at 24, SetLoadCallback at 32.
		if (a_messaging && !g_registerListener) {
			g_registerListener = PatchPointer<RegisterListener_t>(a_messaging, 8, &HookRegisterListener);
		}
		if (a_serialization && !g_setLoadCallback) {
			g_setRevertCallback = PatchPointer<SetCallback_t>(a_serialization, 16, &HookSetRevertCallback);
			g_setLoadCallback = PatchPointer<SetCallback_t>(a_serialization, 32, &HookSetLoadCallback);
		}
	}

	void NoteDataLoaded()
	{
		std::vector<FileOpen> files;
		{
			const std::scoped_lock lock{ g_fileLock };
			if (g_dataLoadedTime != 0) {
				return;
			}
			g_dataLoadedTime = Now();
			files.assign(g_files, g_files + g_fileCount);
		}
		std::sort(files.begin(), files.end(), [](const FileOpen& a_lhs, const FileOpen& a_rhs) { return a_lhs.time < a_rhs.time; });
		for (std::size_t i = 0; i < files.size(); ++i) {
			const LONGLONG end = i + 1 < files.size() ? files[i + 1].time : g_dataLoadedTime;
			Record(files[i].name, "plugin file", Ms(end - files[i].time));
		}
	}

	int SlowFileReads(long long a_from, long long a_to, char* a_out, std::size_t a_cap)
	{
		a_out[0] = '\0';
		int        count = 0;
		std::size_t used = 0;
		const LONG total = g_slowCount;
		for (LONG i = total - 1; i >= 0 && i >= total - kSlowRing && count < 5; --i) {
			const SlowRead& entry = g_slow[i % kSlowRing];
			if (entry.end < a_from || entry.end > a_to) {
				continue;
			}
			const int n = _snprintf_s(a_out + used, a_cap - used, _TRUNCATE, "%s%s %.0f ms%s", count ? ", " : "", entry.name, static_cast<double>(entry.ms),
				entry.open ? " (open)" : "");
			if (n < 0) {
				break;
			}
			used += static_cast<std::size_t>(n);
			++count;
		}
		return count;
	}

	std::vector<Row> Rows()
	{
		std::vector<Row> rows;
		{
			const std::scoped_lock lock{ g_lock };
			rows = g_rows;
		}
		std::sort(rows.begin(), rows.end(), [](const Row& a_lhs, const Row& a_rhs) { return a_lhs.totalMs > a_rhs.totalMs; });
		return rows;
	}
}
