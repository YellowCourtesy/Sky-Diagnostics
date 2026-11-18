#include "HookScan.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <cstring>
#include <iterator>

// Rules for this file: Scan is normal code. The Append functions run after a
// crash: no heap memory, no locks, and memory is read only through
// ReadProcessMemory.

namespace skydiag::crash::hooks
{
	namespace
	{
		struct Hook
		{
			std::uintptr_t at;         // the place in the game code
			std::uintptr_t target;     // where the jump goes (after jumps in trampolines)
			char           owner[96];  // the module of the target at the scan, or ""
		};

		constexpr int kMaxHooks = 8192;
		Hook          g_hooks[kMaxHooks];
		volatile LONG g_count = 0;

		unsigned char* g_file = nullptr;  // the game executable, read from the disk
		DWORD          g_fileSize = 0;
		std::uintptr_t g_base = 0;
		std::uintptr_t g_end = 0;
		bool           g_failed = false;
		char           g_failReason[160];

		bool Read(std::uintptr_t a_address, void* a_out, std::size_t a_size)
		{
			SIZE_T done = 0;
			return a_address >= 0x10000 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(a_address), a_out, a_size, &done) &&
			       done == a_size;
		}

		bool ModuleName(std::uintptr_t a_address, char* a_out, std::size_t a_cap)
		{
			a_out[0] = '\0';
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(a_address),
					&module)) {
				return false;
			}
			wchar_t path[MAX_PATH * 2];
			if (GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path))) == 0) {
				return false;
			}
			const wchar_t* name = wcsrchr(path, L'\\');
			name = name ? name + 1 : path;
			return WideCharToMultiByte(CP_UTF8, 0, name, -1, a_out, static_cast<int>(a_cap), nullptr, nullptr) != 0;
		}

		// A jump instruction at a_address: its target, or 0.
		std::uintptr_t JumpTarget(std::uintptr_t a_address)
		{
			unsigned char code[16];
			if (!Read(a_address, code, sizeof(code))) {
				return 0;
			}
			if (code[0] == 0xE9 || code[0] == 0xE8) {  // jmp rel32, call rel32
				std::int32_t rel;
				std::memcpy(&rel, code + 1, 4);
				return a_address + 5 + static_cast<std::intptr_t>(rel);
			}
			if (code[0] == 0xFF && (code[1] == 0x25 || code[1] == 0x15)) {  // jmp [rip+disp32], call [rip+disp32]
				std::int32_t disp;
				std::memcpy(&disp, code + 2, 4);
				std::uintptr_t target = 0;
				return Read(a_address + 6 + static_cast<std::intptr_t>(disp), &target, sizeof(target)) ? target : 0;
			}
			if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) {  // mov rax, imm64; jmp rax
				std::uintptr_t target;
				std::memcpy(&target, code + 2, 8);
				return target;
			}
			if (code[0] == 0x68 && code[5] == 0xC7 && code[6] == 0x44 && code[7] == 0x24 && code[8] == 0x04 && code[13] == 0xC3) {
				// push imm32; mov dword [rsp+4], imm32; ret
				std::uint32_t low, high;
				std::memcpy(&low, code + 1, 4);
				std::memcpy(&high, code + 9, 4);
				return (static_cast<std::uintptr_t>(high) << 32) | low;
			}
			return 0;
		}

		// The end of a chain of jumps (trampolines), at most 4 steps.
		std::uintptr_t FinalTarget(std::uintptr_t a_target)
		{
			for (int i = 0; i < 4; ++i) {
				const std::uintptr_t next = JumpTarget(a_target);
				if (next == 0) {
					break;
				}
				a_target = next;
			}
			return a_target;
		}

		bool LoadFile()
		{
			if (g_file || g_failed) {
				return g_file != nullptr;
			}
			const HMODULE game = GetModuleHandleW(nullptr);
			MODULEINFO    info{};
			if (!K32GetModuleInformation(GetCurrentProcess(), game, &info, sizeof(info))) {
				g_failed = true;
				strncpy_s(g_failReason, "the game module information is not available", _TRUNCATE);
				return false;
			}
			g_base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
			g_end = g_base + info.SizeOfImage;
			wchar_t path[MAX_PATH * 2];
			if (GetModuleFileNameW(game, path, static_cast<DWORD>(std::size(path))) == 0) {
				g_failed = true;
				strncpy_s(g_failReason, "the game file name is not available", _TRUNCATE);
				return false;
			}
			const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				g_failed = true;
				strncpy_s(g_failReason, "the game file could not be opened", _TRUNCATE);
				return false;
			}
			const DWORD size = GetFileSize(file, nullptr);
			auto*       data = static_cast<unsigned char*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
			DWORD       read = 0;
			const bool  ok = data && ReadFile(file, data, size, &read, nullptr) && read == size;
			CloseHandle(file);
			if (!ok) {
				if (data) {
					VirtualFree(data, 0, MEM_RELEASE);
				}
				g_failed = true;
				strncpy_s(g_failReason, "the game file could not be read", _TRUNCATE);
				return false;
			}
			g_file = data;
			g_fileSize = size;
			return true;
		}

		void Add(std::uintptr_t a_at, std::uintptr_t a_target)
		{
			const LONG count = g_count;
			for (LONG i = 0; i < count; ++i) {
				if (g_hooks[i].at == a_at) {
					if (g_hooks[i].target != a_target) {
						g_hooks[i].target = a_target;
						ModuleName(a_target, g_hooks[i].owner, sizeof(g_hooks[i].owner));
					}
					return;
				}
			}
			if (count >= kMaxHooks) {
				return;
			}
			Hook& h = g_hooks[count];
			h.at = a_at;
			h.target = a_target;
			ModuleName(a_target, h.owner, sizeof(h.owner));
			InterlockedExchange(&g_count, count + 1);
		}

		bool InGame(std::uintptr_t a_address) { return a_address >= g_base && a_address < g_end; }
	}

	int Scan()
	{
		if (!LoadFile()) {
			return 0;
		}
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_file);
		if (g_fileSize < sizeof(IMAGE_DOS_HEADER) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
			static_cast<DWORD>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > g_fileSize) {
			return 0;
		}
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_file + dos->e_lfanew);
		const auto* section = IMAGE_FIRST_SECTION(nt);
		for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
			if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
				continue;
			}
			const DWORD size = (std::min)(section->SizeOfRawData, section->Misc.VirtualSize);
			if (section->PointerToRawData + size > g_fileSize) {
				continue;
			}
			const unsigned char* disk = g_file + section->PointerToRawData;
			const auto*          memory = reinterpret_cast<const unsigned char*>(g_base + section->VirtualAddress);
			// Encrypted or packed code (the file differs everywhere): not usable.
			DWORD differ = 0;
			for (DWORD i = 0; i < size; ++i) {
				if (memory[i] == disk[i]) {
					continue;
				}
				if (++differ > size / 8) {
					break;
				}
				// A hook starts with a jump where the file has different code.
				// The start can be a few bytes before the first different byte (a
				// call or jump whose target was changed differs only after its opcode).
				for (DWORD back = 0; back <= 4 && back <= i; ++back) {
					const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(memory + i - back);
					const std::uintptr_t target = JumpTarget(at);
					if (target != 0 && !InGame(target)) {
						Add(at, FinalTarget(target));
						i += 4;  // past the jump
						break;
					}
				}
			}
		}
		return g_count;
	}

	int AppendNear(Out& a_out, std::uintptr_t a_address)
	{
		int        written = 0;
		const LONG count = g_count;
		for (LONG i = 0; i < count; ++i) {
			const Hook& h = g_hooks[i];
			if (h.at + 64 >= a_address && h.at <= a_address + 16) {
				Put(a_out, "  A hook of %s is at the game code +%llX, %lld bytes from the crash place.\n", h.owner[0] ? h.owner : "(code not in a module)",
					static_cast<unsigned long long>(h.at - g_base), static_cast<long long>(static_cast<std::intptr_t>(a_address - h.at)));
				++written;
			}
		}
		return written;
	}

	int AppendTo(Out& a_out, std::uintptr_t a_target)
	{
		int        written = 0;
		const LONG count = g_count;
		for (LONG i = 0; i < count; ++i) {
			const Hook& h = g_hooks[i];
			if (h.target == a_target || (a_target >= h.target && a_target < h.target + 64)) {
				Put(a_out, "  The hook at the game code +%llX went to this place: it was a hook of %s.\n", static_cast<unsigned long long>(h.at - g_base),
					h.owner[0] ? h.owner : "code that was not in a module");
				++written;
			}
		}
		return written;
	}

	void AppendSummary(Out& a_out)
	{
		const LONG count = g_count;
		Put(a_out, "\n== HOOKS IN THE GAME CODE (%ld) ==\n", static_cast<long>(count));
		if (g_failed) {
			Put(a_out, "Not available: %s.\n", g_failReason);
			return;
		}
		// Counts by owner, no heap: at most 64 owners.
		static const char* owners[64];
		static int         counts[64];
		int                ownerCount = 0;
		for (LONG i = 0; i < count; ++i) {
			const char* owner = g_hooks[i].owner[0] ? g_hooks[i].owner : "(code not in a module)";
			int         k = 0;
			for (; k < ownerCount && _stricmp(owners[k], owner) != 0; ++k) {
			}
			if (k == ownerCount && ownerCount < 64) {
				owners[ownerCount] = owner;
				counts[ownerCount++] = 0;
			}
			if (k < ownerCount) {
				++counts[k];
			}
		}
		for (int k = 0; k < ownerCount; ++k) {
			Put(a_out, "%-48s %d hook%s\n", owners[k], counts[k], counts[k] == 1 ? "" : "s");
		}
		// Hooks that go to code that is not loaded now (a DLL was unloaded).
		for (LONG i = 0; i < count; ++i) {
			const Hook&             h = g_hooks[i];
			MEMORY_BASIC_INFORMATION info{};
			if (VirtualQuery(reinterpret_cast<LPCVOID>(h.target), &info, sizeof(info)) == sizeof(info) && info.State == MEM_FREE) {
				Put(a_out, "The hook at the game code +%llX of %s goes to memory that is free now: that code was unloaded.\n",
					static_cast<unsigned long long>(h.at - g_base), h.owner[0] ? h.owner : "(code not in a module)");
			}
		}
	}
}
