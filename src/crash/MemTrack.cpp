#include "MemTrack.h"

#include "CrashHandler.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstring>
#include <iterator>

namespace skydiag::memtrack
{
	namespace
	{
		constexpr int kMaxTracked = 1024;

		struct Tracked
		{
			std::uintptr_t             base;
			std::uintptr_t             size;
			char                       name[96];
			std::atomic<std::int64_t>  inUse;
			std::atomic<std::uint64_t> allocated;
			std::atomic<std::uint64_t> allocations;
		};

		// Entries are only added, so an index stays valid. The order tables
		// give the entries sorted by address, for a fast search. A writer
		// fills the inactive order table and then switches.
		Tracked          g_tracked[kMaxTracked];
		std::atomic<int> g_trackedCount{ 0 };
		std::uint16_t    g_order[2][kMaxTracked];
		std::atomic<int> g_orderCount[2];
		std::atomic<int> g_activeOrder{ 0 };
		SRWLOCK          g_hookLock = SRWLOCK_INIT;

		// The real functions.
		using MallocFn = void* (__cdecl*)(std::size_t);
		using CallocFn = void* (__cdecl*)(std::size_t, std::size_t);
		using ReallocFn = void* (__cdecl*)(void*, std::size_t);
		using FreeFn = void(__cdecl*)(void*);
		using MsizeFn = std::size_t(__cdecl*)(void*);

		void*     g_realHeapAlloc = nullptr;
		void*     g_realHeapReAlloc = nullptr;
		void*     g_realHeapFree = nullptr;
		MallocFn  g_realMalloc = nullptr;
		CallocFn  g_realCalloc = nullptr;
		ReallocFn g_realRealloc = nullptr;
		FreeFn    g_realFree = nullptr;
		MsizeFn   g_realMsize = nullptr;

		Tracked* FindTracked(std::uintptr_t a_address)
		{
			const int            active = g_activeOrder.load(std::memory_order_acquire);
			const std::uint16_t* order = g_order[active];
			int                  low = 0, high = g_orderCount[active].load(std::memory_order_relaxed);
			while (low < high) {
				const int middle = (low + high) / 2;
				Tracked&  t = g_tracked[order[middle]];
				if (a_address < t.base) {
					high = middle;
				} else if (a_address >= t.base + t.size) {
					low = middle + 1;
				} else {
					return &t;
				}
			}
			return nullptr;
		}

		// The DLL of a call. Each DLL has its own entry code for each heap
		// function, and that code gives the DLL number in the fourth
		// parameter. The return address is not used for this: a compiler
		// often changes a call of free at the end of a function to a jump,
		// and then the return address is in a different module.
		Tracked* ByIndex(std::uint64_t a_index)
		{
			return a_index < static_cast<std::uint64_t>(g_trackedCount.load(std::memory_order_relaxed)) ? &g_tracked[a_index] : nullptr;
		}

		void Note(Tracked* a_tracked, std::int64_t a_change)
		{
			if (Tracked* t = a_tracked) {
				t->inUse.fetch_add(a_change, std::memory_order_relaxed);
				if (a_change > 0) {
					t->allocated.fetch_add(static_cast<std::uint64_t>(a_change), std::memory_order_relaxed);
					t->allocations.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}

		LPVOID WINAPI HookHeapAlloc(HANDLE a_heap, DWORD a_flags, SIZE_T a_bytes, std::uint64_t a_dll)
		{
			const LPVOID result = HeapAlloc(a_heap, a_flags, a_bytes);
			if (result) {
				Note(ByIndex(a_dll), static_cast<std::int64_t>(a_bytes));
			}
			return result;
		}

		// HeapReAlloc has four parameters, so there is no free register
		// for the DLL number. This function finds the DLL from the return
		// address.
		LPVOID WINAPI HookHeapReAlloc(HANDLE a_heap, DWORD a_flags, LPVOID a_block, SIZE_T a_bytes)
		{
			const SIZE_T before = a_block ? HeapSize(a_heap, 0, a_block) : 0;
			const LPVOID result = HeapReAlloc(a_heap, a_flags, a_block, a_bytes);
			if (result && before != static_cast<SIZE_T>(-1)) {
				Tracked* const caller = FindTracked(reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
				if (before != 0) {
					Note(caller, -static_cast<std::int64_t>(before));
				}
				Note(caller, static_cast<std::int64_t>(a_bytes));
			}
			return result;
		}

		BOOL WINAPI HookHeapFree(HANDLE a_heap, DWORD a_flags, LPVOID a_block, std::uint64_t a_dll)
		{
			if (a_block) {
				const SIZE_T size = HeapSize(a_heap, 0, a_block);
				if (size != static_cast<SIZE_T>(-1) && size != 0) {
					Note(ByIndex(a_dll), -static_cast<std::int64_t>(size));
				}
			}
			return HeapFree(a_heap, a_flags, a_block);
		}

		// The C functions have one or two parameters. The entry code gives
		// the DLL number in the fourth parameter; the parameters between
		// are not used.
		void* __cdecl HookMalloc(std::size_t a_bytes, void*, void*, std::uint64_t a_dll)
		{
			void* result = g_realMalloc(a_bytes);
			if (result) {
				Note(ByIndex(a_dll), static_cast<std::int64_t>(a_bytes));
			}
			return result;
		}

		void* __cdecl HookCalloc(std::size_t a_count, std::size_t a_size, void*, std::uint64_t a_dll)
		{
			void* result = g_realCalloc(a_count, a_size);
			if (result) {
				Note(ByIndex(a_dll), static_cast<std::int64_t>(a_count * a_size));
			}
			return result;
		}

		void* __cdecl HookRealloc(void* a_block, std::size_t a_bytes, void*, std::uint64_t a_dll)
		{
			const std::size_t before = a_block ? g_realMsize(a_block) : 0;
			void*             result = g_realRealloc(a_block, a_bytes);
			Tracked* const    caller = ByIndex(a_dll);
			// realloc with size 0 releases the block and gives no new one.
			if (before != 0 && before != static_cast<std::size_t>(-1) && (result || a_bytes == 0)) {
				Note(caller, -static_cast<std::int64_t>(before));
			}
			if (result && a_bytes != 0) {
				Note(caller, static_cast<std::int64_t>(a_bytes));
			}
			return result;
		}

		void __cdecl HookFree(void* a_block, void*, void*, std::uint64_t a_dll)
		{
			if (a_block) {
				const std::size_t size = g_realMsize(a_block);
				if (size != static_cast<std::size_t>(-1) && size != 0) {
					Note(ByIndex(a_dll), -static_cast<std::int64_t>(size));
				}
			}
			g_realFree(a_block);
		}

		// Entry code. For each DLL and each counting function with a DLL
		// number there is one block of machine code:
		//   mov r9d, <DLL number>
		//   mov rax, <counting function>
		//   jmp rax
		// r9 is the register of the fourth parameter. The heap functions
		// here have three parameters at most, so r9 is free.
		constexpr int         kEntryFunctions = 6;
		constexpr std::size_t kEntrySize = 24;
		std::uint8_t*         g_entries = nullptr;

		enum EntryFunction
		{
			kEntryHeapAlloc,
			kEntryHeapFree,
			kEntryMalloc,
			kEntryCalloc,
			kEntryRealloc,
			kEntryFree,
		};

		bool MakeEntries(int a_dll)
		{
			if (!g_entries) {
				g_entries = static_cast<std::uint8_t*>(
					VirtualAlloc(nullptr, kMaxTracked * kEntryFunctions * kEntrySize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
				if (!g_entries) {
					return false;
				}
			}
			void* const targets[kEntryFunctions] = { reinterpret_cast<void*>(&HookHeapAlloc), reinterpret_cast<void*>(&HookHeapFree),
				reinterpret_cast<void*>(&HookMalloc), reinterpret_cast<void*>(&HookCalloc), reinterpret_cast<void*>(&HookRealloc),
				reinterpret_cast<void*>(&HookFree) };
			for (int f = 0; f < kEntryFunctions; ++f) {
				std::uint8_t*       code = g_entries + (static_cast<std::size_t>(a_dll) * kEntryFunctions + f) * kEntrySize;
				const std::uint32_t index = static_cast<std::uint32_t>(a_dll);
				const std::uint64_t target = reinterpret_cast<std::uint64_t>(targets[f]);
				code[0] = 0x41;  // mov r9d, imm32
				code[1] = 0xB9;
				std::memcpy(code + 2, &index, 4);
				code[6] = 0x48;  // mov rax, imm64
				code[7] = 0xB8;
				std::memcpy(code + 8, &target, 8);
				code[16] = 0xFF;  // jmp rax
				code[17] = 0xE0;
				std::memset(code + 18, 0xCC, kEntrySize - 18);
			}
			FlushInstructionCache(GetCurrentProcess(), g_entries + static_cast<std::size_t>(a_dll) * kEntryFunctions * kEntrySize,
				kEntryFunctions * kEntrySize);
			return true;
		}

		void* EntryCode(int a_dll, EntryFunction a_function)
		{
			return g_entries + (static_cast<std::size_t>(a_dll) * kEntryFunctions + a_function) * kEntrySize;
		}

		bool FindRealFunctions()
		{
			if (g_realHeapAlloc) {
				return true;
			}
			const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
			const HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
			if (!kernel) {
				return false;
			}
			g_realHeapReAlloc = reinterpret_cast<void*>(GetProcAddress(kernel, "HeapReAlloc"));
			g_realHeapFree = reinterpret_cast<void*>(GetProcAddress(kernel, "HeapFree"));
			if (ucrt) {
				g_realMsize = reinterpret_cast<MsizeFn>(GetProcAddress(ucrt, "_msize"));
				g_realFree = reinterpret_cast<FreeFn>(GetProcAddress(ucrt, "free"));
				g_realRealloc = reinterpret_cast<ReallocFn>(GetProcAddress(ucrt, "realloc"));
				g_realCalloc = reinterpret_cast<CallocFn>(GetProcAddress(ucrt, "calloc"));
				g_realMalloc = reinterpret_cast<MallocFn>(GetProcAddress(ucrt, "malloc"));
				// All or nothing: a free with no count for its malloc gives wrong numbers.
				if (!g_realMsize || !g_realFree || !g_realRealloc || !g_realCalloc || !g_realMalloc) {
					g_realMalloc = nullptr;
				}
			}
			g_realHeapAlloc = reinterpret_cast<void*>(GetProcAddress(kernel, "HeapAlloc"));
			return g_realHeapAlloc && g_realHeapReAlloc && g_realHeapFree;
		}

		// The code that replaces a real function for the DLL with the
		// number a_dll, or nullptr.
		void* HookFor(void* a_real, int a_dll)
		{
			if (!a_real) {
				return nullptr;
			}
			if (a_real == g_realHeapAlloc) {
				return EntryCode(a_dll, kEntryHeapAlloc);
			}
			if (a_real == g_realHeapReAlloc) {
				return reinterpret_cast<void*>(&HookHeapReAlloc);
			}
			if (a_real == g_realHeapFree) {
				return EntryCode(a_dll, kEntryHeapFree);
			}
			if (g_realMalloc) {
				if (a_real == reinterpret_cast<void*>(g_realMalloc)) {
					return EntryCode(a_dll, kEntryMalloc);
				}
				if (a_real == reinterpret_cast<void*>(g_realCalloc)) {
					return EntryCode(a_dll, kEntryCalloc);
				}
				if (a_real == reinterpret_cast<void*>(g_realRealloc)) {
					return EntryCode(a_dll, kEntryRealloc);
				}
				if (a_real == reinterpret_cast<void*>(g_realFree)) {
					return EntryCode(a_dll, kEntryFree);
				}
			}
			return nullptr;
		}

		// Changes the import table of one DLL. Only entries that point to
		// a real heap function are changed: an entry that a different tool
		// already changed stays as it is. Returns the number of changes.
		int PatchImports(std::uintptr_t a_base, std::uintptr_t a_size, int a_dll)
		{
			int patched = 0;
			__try {
				const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(a_base);
				if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
					return 0;
				}
				const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(a_base + dos->e_lfanew);
				if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
					return 0;
				}
				const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
				if (dir.VirtualAddress == 0 || dir.Size == 0 || dir.VirtualAddress >= a_size) {
					return 0;
				}
				auto descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(a_base + dir.VirtualAddress);
				for (int d = 0; d < 4096 && descriptor->Name != 0; ++d, ++descriptor) {
					if (descriptor->FirstThunk == 0 || descriptor->FirstThunk >= a_size) {
						continue;
					}
					auto slot = reinterpret_cast<void**>(a_base + descriptor->FirstThunk);
					for (int i = 0; i < 65536 && *slot != nullptr; ++i, ++slot) {
						void* const hook = HookFor(*slot, a_dll);
						if (!hook) {
							continue;
						}
						DWORD oldProtect = 0;
						if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
							InterlockedExchangePointer(slot, hook);
							VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
							++patched;
						}
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return patched;
		}

		void MegabytesText(char* a_out, std::size_t a_cap, std::int64_t a_bytes)
		{
			const double megabytes = static_cast<double>(a_bytes) / (1024.0 * 1024.0);
			if (megabytes >= 1024.0 || megabytes <= -1024.0) {
				_snprintf_s(a_out, a_cap, _TRUNCATE, "%.0f MB (%.1f GB)", megabytes, megabytes / 1024.0);
			} else {
				_snprintf_s(a_out, a_cap, _TRUNCATE, "%.1f MB", megabytes);
			}
		}
	}

	int HookModules()
	{
		static crash::ModDll mods[kMaxTracked];

		AcquireSRWLockExclusive(&g_hookLock);
		if (!FindRealFunctions()) {
			ReleaseSRWLockExclusive(&g_hookLock);
			return 0;
		}
		const int   modCount = crash::ListModDlls(mods, kMaxTracked);
		const int   before = g_trackedCount.load();
		int         count = before;
		for (int i = 0; i < modCount; ++i) {
			bool known = false;
			for (int t = 0; t < count; ++t) {
				known = known || g_tracked[t].base == mods[i].base;
			}
			if (known || count >= kMaxTracked) {
				continue;
			}
			Tracked& t = g_tracked[count];
			t.base = mods[i].base;
			t.size = mods[i].size;
			strncpy_s(t.name, mods[i].name, _TRUNCATE);
			++count;
		}

		// The order table first, so that the counting functions find the
		// new DLLs as soon as their import tables change.
		const int      inactive = 1 - g_activeOrder.load();
		std::uint16_t* order = g_order[inactive];
		for (int i = 0; i < count; ++i) {
			order[i] = static_cast<std::uint16_t>(i);
		}
		for (int i = 1; i < count; ++i) {
			const std::uint16_t value = order[i];
			int                 j = i - 1;
			for (; j >= 0 && g_tracked[order[j]].base > g_tracked[value].base; --j) {
				order[j + 1] = order[j];
			}
			order[j + 1] = value;
		}
		g_orderCount[inactive].store(count);
		g_trackedCount.store(count);
		g_activeOrder.store(inactive, std::memory_order_release);

		for (int i = before; i < count; ++i) {
			if (MakeEntries(i)) {
				PatchImports(g_tracked[i].base, g_tracked[i].size, i);
			}
		}
		ReleaseSRWLockExclusive(&g_hookLock);
		return count;
	}

	int GetTop(Entry* a_out, int a_max)
	{
		const int count = g_trackedCount.load();
		int       used = 0;
		for (int i = 0; i < count; ++i) {
			const Tracked&     t = g_tracked[i];
			const std::int64_t inUse = t.inUse.load(std::memory_order_relaxed);
			if (t.allocations.load(std::memory_order_relaxed) == 0) {
				continue;
			}
			// Insert in the sorted result.
			int position = used;
			while (position > 0 && a_out[position - 1].inUseBytes < inUse) {
				--position;
			}
			if (position >= a_max) {
				continue;
			}
			const int last = used < a_max ? used : a_max - 1;
			for (int j = last; j > position; --j) {
				a_out[j] = a_out[j - 1];
			}
			Entry& e = a_out[position];
			strncpy_s(e.name, t.name, _TRUNCATE);
			e.inUseBytes = inUse;
			e.allocatedBytes = t.allocated.load(std::memory_order_relaxed);
			e.allocations = t.allocations.load(std::memory_order_relaxed);
			if (used < a_max) {
				++used;
			}
		}
		return used;
	}

	void Append(Out& a_out)
	{
		if (g_trackedCount.load() == 0) {
			return;
		}
		static Entry entries[15];
		const int    count = GetTop(entries, static_cast<int>(std::size(entries)));
		Put(a_out, "\n== HEAP MEMORY BY MOD DLL ==\n");
		Put(a_out, "Heap memory that each mod DLL allocated and did not release (%d DLLs tracked). Memory from the game's own\n", g_trackedCount.load());
		Put(a_out, "memory manager, VirtualAlloc and the graphics driver is not included.\n");
		if (count == 0) {
			Put(a_out, "No mod DLL allocated heap memory.\n");
			return;
		}
		Put(a_out, "In use              All allocations     Count       Mod DLL\n");
		for (int i = 0; i < count; ++i) {
			char inUse[40], total[40];
			MegabytesText(inUse, sizeof(inUse), entries[i].inUseBytes);
			MegabytesText(total, sizeof(total), static_cast<std::int64_t>(entries[i].allocatedBytes));
			Put(a_out, "%-18s  %-18s  %-10llu  %s\n", inUse, total, static_cast<unsigned long long>(entries[i].allocations), entries[i].name);
		}
	}
}
