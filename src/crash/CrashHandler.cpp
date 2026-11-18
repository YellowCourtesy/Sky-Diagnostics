#include "CrashHandler.h"

#include "HookScan.h"
#include "MemTrack.h"
#include "Monitor.h"
#include "Phostwood.h"

#include "Profiler.h"
#include "Session.h"
#include "Text.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>

// Rules for this file:
// - Code that runs after a crash must not allocate heap memory and must not
//   take the loader lock. The crashed thread can hold those locks.
// - All memory of unknown validity is read through SafeRead.

namespace skydiag::crash
{
	namespace
	{
		// ---------------------------------------------------------------- data

		struct Module
		{
			std::uintptr_t base;
			std::uintptr_t size;
			char           name[96];
			bool           isSystem;
			bool           isFramework;  // script extender, Steam, video player: not the game, not a mod
			bool           isGame;
			bool           isSelf;
		};

		constexpr int         kMaxModules = 2048;
		constexpr int         kMaxFrames = 128;
		constexpr int         kStackSlots = 160;
		constexpr std::size_t kReportCap = 1024 * 1024;
		constexpr DWORD       kCppException = 0xE06D7363;

		struct ModuleTable
		{
			Module items[kMaxModules];
			int    count;
		};

		// Two tables. Writers fill the inactive one and then switch.
		ModuleTable      g_tables[2];
		std::atomic<int> g_activeTable{ 0 };
		SRWLOCK          g_refreshLock = SRWLOCK_INIT;

		wchar_t  g_reportDir[600];
		char     g_toolVersion[64];
		char     g_gameVersion[64];
		char     g_platform[128];
		DWORD    g_mainThreadId = 0;
		bool     g_writeMinidump = true;
		ULONGLONG g_startTick = 0;

		HANDLE g_goEvent = nullptr;
		HANDLE g_doneEvent = nullptr;
		volatile LONG g_inCrash = 0;
		EXCEPTION_POINTERS* volatile g_crashPointers = nullptr;
		volatile DWORD g_crashThreadId = 0;

		std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_previousFilter{ nullptr };

		// The short answer at the top of a crash report (filled by BuildReport,
		// written by WriteFiles).
		struct Quick
		{
			char        what[600];
			char        suspect[320];
			const char* confidence;
			char        tryText[600];
			char        group[200];  // the place of the crash, for the crash history
			bool        closing;     // a crash while the game closes
		};
		Quick g_quick;
		bool  g_openReport = true;

		// Exceptions that code caught: access violations and the like that did
		// not end the game. A vectored handler sees them first. Many of them can
		// mean that a DLL hides crashes.
		struct Caught
		{
			ULONGLONG      tick;
			std::uintptr_t address;
			DWORD          code;
			DWORD          threadId;
		};
		constexpr int kCaughtRing = 32;
		Caught        g_caught[kCaughtRing];
		volatile LONG g_caughtCount = 0;

		char g_report[kReportCap];
		char g_stallReport[kReportCap];
		char g_note[256];
		char g_save[300];
		char g_setup[300];

		constexpr int kMaxThreads = 1024;
		struct ThreadTable
		{
			DWORD ids[kMaxThreads];
			int   count;
		};
		ThreadTable      g_threadTables[2];
		std::atomic<int> g_activeThreadTable{ 0 };
		volatile LONG    g_inStall = 0;

		struct DescriberEntry
		{
			char      baseClass[64];
			Describer function;
		};
		struct SectionEntry
		{
			char            title[48];
			SectionFunction function;
		};
		SectionEntry     g_sections[8];
		std::atomic<int> g_sectionCount{ 0 };

		DescriberEntry   g_describers[16];
		std::atomic<int> g_describerCount{ 0 };

		// ------------------------------------------------------------- memory

		bool SafeRead(std::uintptr_t a_address, void* a_out, std::size_t a_size)
		{
			if (a_address < 0x10000) {
				return false;
			}
			SIZE_T done = 0;
			return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(a_address), a_out, a_size, &done) && done == a_size;
		}

		template <class T>
		bool SafeGet(std::uintptr_t a_address, T& a_out)
		{
			return SafeRead(a_address, &a_out, sizeof(T));
		}

		// ------------------------------------------------------------ modules

		const ModuleTable& Modules()
		{
			return g_tables[g_activeTable.load(std::memory_order_acquire)];
		}

		const Module* FindModule(std::uintptr_t a_address)
		{
			const auto& table = Modules();
			for (int i = 0; i < table.count; ++i) {
				const auto& m = table.items[i];
				if (a_address >= m.base && a_address < m.base + m.size) {
					return &m;
				}
			}
			return nullptr;
		}

		bool StartsWithNoCase(const wchar_t* a_text, const wchar_t* a_prefix)
		{
			const std::size_t n = wcslen(a_prefix);
			return n > 0 && _wcsnicmp(a_text, a_prefix, n) == 0;
		}

		// Finds the unwind data of a function in the module's own table.
		// RtlLookupFunctionEntry is not used: it takes a lock, and a crashed
		// or suspended thread can hold that lock.
		PRUNTIME_FUNCTION LookupFunction(std::uintptr_t a_rip, DWORD64& a_imageBase)
		{
			const Module* m = FindModule(a_rip);
			if (!m) {
				return nullptr;
			}
			const auto base = m->base;
			const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
				return nullptr;
			}
			const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) {
				return nullptr;
			}
			const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
			if (dir.VirtualAddress == 0 || dir.Size < sizeof(RUNTIME_FUNCTION)) {
				return nullptr;
			}
			const auto  table = reinterpret_cast<PRUNTIME_FUNCTION>(base + dir.VirtualAddress);
			const DWORD rva = static_cast<DWORD>(a_rip - base);
			DWORD       lo = 0;
			DWORD       hi = dir.Size / sizeof(RUNTIME_FUNCTION);
			while (lo < hi) {
				const DWORD mid = lo + (hi - lo) / 2;
				if (rva < table[mid].BeginAddress) {
					hi = mid;
				} else if (rva >= table[mid].EndAddress) {
					lo = mid + 1;
				} else {
					a_imageBase = base;
					return &table[mid];
				}
			}
			return nullptr;
		}

		// Finds the exported function at or before an address. Used only
		// for system DLLs, where exports cover most functions.
		bool ExportName(const Module& a_module, std::uintptr_t a_address, char* a_out, std::size_t a_cap, std::uintptr_t& a_offset)
		{
			__try {
				const auto base = a_module.base;
				const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
				if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
					return false;
				}
				const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
				const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
				if (nt->Signature != IMAGE_NT_SIGNATURE || dir.VirtualAddress == 0) {
					return false;
				}
				const auto exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
				const auto names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
				const auto ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
				const auto functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);

				const DWORD rva = static_cast<DWORD>(a_address - base);
				DWORD       bestRva = 0;
				DWORD       bestName = 0;
				for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
					if (ordinals[i] >= exports->NumberOfFunctions) {
						continue;
					}
					const DWORD functionRva = functions[ordinals[i]];
					if (functionRva <= rva && functionRva > bestRva) {
						bestRva = functionRva;
						bestName = names[i];
					}
				}
				if (bestName == 0 || rva - bestRva > 0x4000) {
					return false;
				}
				strncpy_s(a_out, a_cap, reinterpret_cast<const char*>(base + bestName), _TRUNCATE);
				a_offset = rva - bestRva;
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return false;
		}

		// The C and C++ runtime DLLs are not always in the Windows folder
		// (a game or a mod manager can supply them). They are never the mod
		// that the report must name.
		bool IsRuntimeLibrary(const wchar_t* a_baseName)
		{
			return StartsWithNoCase(a_baseName, L"vcruntime") || StartsWithNoCase(a_baseName, L"msvcp") ||
			       StartsWithNoCase(a_baseName, L"ucrtbase") || StartsWithNoCase(a_baseName, L"concrt") ||
			       StartsWithNoCase(a_baseName, L"vccorlib");
		}

		// DLLs that are on almost all call stacks because they call mod
		// code or load with the game. The report does not name them as the
		// probable source when a mod DLL is not on the call stack.
		bool IsFrameworkLibrary(const wchar_t* a_baseName)
		{
			static const wchar_t* const prefixes[] = { L"skse64", L"sksevr", L"steam_api", L"steamclient", L"gameoverlayrenderer",
				L"bink2w", L"tbb", L"openvr_api" };
			for (const auto prefix : prefixes) {
				if (StartsWithNoCase(a_baseName, prefix)) {
					return true;
				}
			}
			return false;
		}

		std::atomic<ModLookup> g_modLookup{ nullptr };

		// The mod that a DLL comes from, or "". Mod DLLs only.
		const char* ModOf(const Module& a_module)
		{
			const ModLookup lookup = g_modLookup.load();
			if (!lookup || a_module.isSystem || a_module.isGame || a_module.isSelf || a_module.isFramework) {
				return "";
			}
			const char* mod = lookup(a_module.name);
			return mod ? mod : "";
		}

		// "Foo.dll (mod: Foo Mod)" if the mod is known, or "Foo.dll". The
		// text is in one static buffer: use it before the next call.
		const char* NameWithMod(const Module& a_module)
		{
			static char text[320];
			const char* mod = ModOf(a_module);
			if (mod[0] == '\0') {
				return a_module.name;
			}
			_snprintf_s(text, sizeof(text), _TRUNCATE, "%s (mod: %s)", a_module.name, mod);
			return text;
		}

		const char* ModuleTag(const Module& a_module)
		{
			return a_module.isGame ? " [game]" : (a_module.isSelf ? " [this tool]" : (a_module.isSystem ? " [system]" : (a_module.isFramework ? " [framework]" : "")));
		}

		bool IsModLibrary(const Module* a_module)
		{
			return a_module && !a_module->isSystem && !a_module->isGame && !a_module->isSelf && !a_module->isFramework;
		}

		// ---------------------------------------------------------------- RTTI

		bool IsIdentifierChar(char a_char)
		{
			return (a_char >= 'a' && a_char <= 'z') || (a_char >= 'A' && a_char <= 'Z') || (a_char >= '0' && a_char <= '9') || a_char == '_';
		}

		// Reads "name@scope@...@@" (a qualified name in compiler form) and
		// writes "scope::name". Returns the position after the name, or
		// nullptr if the text is not a plain qualified name.
		const char* ReadQualifiedName(const char* a_text, char* a_out, std::size_t a_cap)
		{
			const char* parts[16];
			std::size_t lens[16];
			int         n = 0;
			const char* p = a_text;
			while (*p != '@') {
				const char* start = p;
				while (IsIdentifierChar(*p)) {
					++p;
				}
				if (p == start || *p != '@' || n >= 16) {
					return nullptr;
				}
				parts[n] = start;
				lens[n] = static_cast<std::size_t>(p - start);
				++n;
				++p;
			}
			a_out[0] = '\0';
			for (int i = n - 1; i >= 0; --i) {
				strncat_s(a_out, a_cap, parts[i], lens[i]);
				if (i > 0) {
					strncat_s(a_out, a_cap, "::", _TRUNCATE);
				}
			}
			return p + 1;
		}

		// Changes the compiler form of a class name into a readable form:
		//   .?AVname@scope@@             -> scope::name
		//   .?AV?$name@Varg@@@scope@@    -> scope::name<arg>
		//   other template names         -> name<...>
		void Demangle(const char* a_raw, char* a_out, std::size_t a_cap)
		{
			const char* p = a_raw + 4;
			if (p[0] != '?') {
				if (ReadQualifiedName(p, a_out, a_cap) == nullptr || a_out[0] == '\0') {
					strncpy_s(a_out, a_cap, a_raw, _TRUNCATE);
				}
				return;
			}
			if (p[1] != '$') {
				strncpy_s(a_out, a_cap, a_raw, _TRUNCATE);
				return;
			}

			// Template: the name, then the arguments, then '@', then the scope.
			const char* name = p + 2;
			const char* nameEnd = name;
			while (IsIdentifierChar(*nameEnd)) {
				++nameEnd;
			}
			if (nameEnd == name || *nameEnd != '@') {
				strncpy_s(a_out, a_cap, a_raw, _TRUNCATE);
				return;
			}
			const char* args = nameEnd + 1;

			char argument[96] = "...";
			char scope[96] = "";
			if (args[0] == 'V' || args[0] == 'U') {
				char        single[96];
				const char* afterArgument = ReadQualifiedName(args + 1, single, sizeof(single));
				if (afterArgument && *afterArgument == '@') {
					// Exactly one class argument: the scope follows.
					strncpy_s(argument, single, _TRUNCATE);
					char parsed[96];
					if (ReadQualifiedName(afterArgument + 1, parsed, sizeof(parsed))) {
						strncpy_s(scope, parsed, _TRUNCATE);
					}
				}
			}

			a_out[0] = '\0';
			if (scope[0] != '\0') {
				strncat_s(a_out, a_cap, scope, _TRUNCATE);
				strncat_s(a_out, a_cap, "::", _TRUNCATE);
			}
			strncat_s(a_out, a_cap, name, static_cast<std::size_t>(nameEnd - name));
			strncat_s(a_out, a_cap, "<", _TRUNCATE);
			strncat_s(a_out, a_cap, argument, _TRUNCATE);
			strncat_s(a_out, a_cap, ">", _TRUNCATE);
		}

		bool ReadTypeName(std::uintptr_t a_typeDescriptor, char* a_out, std::size_t a_cap)
		{
			char raw[160] = {};
			// The name starts at offset 0x10 of the type descriptor.
			if (!SafeRead(a_typeDescriptor + 0x10, raw, 8)) {
				return false;
			}
			if (raw[0] != '.' || raw[1] != '?' || raw[2] != 'A') {
				return false;
			}
			std::size_t n = 8;
			while (n < sizeof(raw) - 1) {
				if (!SafeRead(a_typeDescriptor + 0x10 + n, raw + n, 1) || raw[n] == '\0') {
					break;
				}
				++n;
			}
			raw[n] = '\0';
			Demangle(raw, a_out, a_cap);
			return true;
		}

		struct RttiInfo
		{
			char           name[128];
			std::uintptr_t complete;       // address of the complete object
			std::uintptr_t moduleBase;
			std::uintptr_t moduleSize;
			std::uint32_t  classDescriptorRva;
		};

		// If a_object points to a C++ object with a virtual table, this gets
		// the class name from the MSVC run-time type data.
		bool RttiLookup(std::uintptr_t a_object, RttiInfo& a_info)
		{
			std::uintptr_t vtable = 0;
			if (!SafeGet(a_object, vtable)) {
				return false;
			}
			const Module* mod = FindModule(vtable);
			if (!mod) {
				return false;
			}
			std::uintptr_t locator = 0;
			if (!SafeGet(vtable - sizeof(std::uintptr_t), locator) || FindModule(locator) != mod) {
				return false;
			}
			struct CompleteObjectLocator
			{
				std::uint32_t signature;
				std::uint32_t offset;
				std::uint32_t cdOffset;
				std::uint32_t typeDescriptorRva;
				std::uint32_t classDescriptorRva;
				std::uint32_t selfRva;
			} col{};
			if (!SafeGet(locator, col) || col.signature != 1 || locator - col.selfRva != mod->base) {
				return false;
			}
			if (col.typeDescriptorRva >= mod->size || col.classDescriptorRva >= mod->size) {
				return false;
			}
			if (!ReadTypeName(mod->base + col.typeDescriptorRva, a_info.name, sizeof(a_info.name))) {
				return false;
			}
			a_info.complete = a_object - col.offset;
			a_info.moduleBase = mod->base;
			a_info.moduleSize = mod->size;
			a_info.classDescriptorRva = col.classDescriptorRva;
			return true;
		}

		bool CallDescriber(Describer a_function, std::uintptr_t a_subobject, char* a_text, std::size_t a_textCap, char* a_source,
			std::size_t a_sourceCap)
		{
			__try {
				a_function(reinterpret_cast<void*>(a_subobject), a_text, a_textCap, a_source, a_sourceCap);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return false;
		}

		// Looks at the base classes of the object. If a describer is
		// registered for one of them, calls it with the address of that base
		// class part of the object.
		bool Describe(const RttiInfo& a_info, char* a_text, std::size_t a_textCap, char* a_source, std::size_t a_sourceCap)
		{
			a_text[0] = '\0';
			a_source[0] = '\0';
			const int describerCount = g_describerCount.load();
			if (describerCount == 0) {
				return false;
			}
			struct ClassHierarchy
			{
				std::uint32_t signature;
				std::uint32_t attributes;
				std::uint32_t baseCount;
				std::uint32_t baseArrayRva;
			} hierarchy{};
			if (!SafeGet(a_info.moduleBase + a_info.classDescriptorRva, hierarchy) || hierarchy.baseCount == 0 || hierarchy.baseCount > 64 ||
				hierarchy.baseArrayRva >= a_info.moduleSize) {
				return false;
			}
			for (std::uint32_t i = 0; i < hierarchy.baseCount; ++i) {
				std::uint32_t baseRva = 0;
				struct BaseClass
				{
					std::uint32_t typeDescriptorRva;
					std::uint32_t containedBases;
					std::int32_t  memberOffset;
					std::int32_t  vbtableOffset;
					std::int32_t  offsetInVbtable;
					std::uint32_t attributes;
				} base{};
				if (!SafeGet(a_info.moduleBase + hierarchy.baseArrayRva + i * 4, baseRva) || baseRva >= a_info.moduleSize ||
					!SafeGet(a_info.moduleBase + baseRva, base) || base.typeDescriptorRva >= a_info.moduleSize) {
					return false;
				}
				if (base.vbtableOffset != -1) {
					continue;  // virtual base class: not supported
				}
				char name[128];
				if (!ReadTypeName(a_info.moduleBase + base.typeDescriptorRva, name, sizeof(name))) {
					continue;
				}
				for (int d = 0; d < describerCount; ++d) {
					if (strcmp(g_describers[d].baseClass, name) == 0) {
						const bool ok = CallDescriber(g_describers[d].function, a_info.complete + base.memberOffset, a_text, a_textCap,
							a_source, a_sourceCap);
						if (!ok) {
							strncpy_s(a_text, a_textCap, "(object data is not readable)", _TRUNCATE);
							a_source[0] = '\0';
						}
						return a_text[0] != '\0';
					}
				}
			}
			return false;
		}

		// ------------------------------------------------------------- output

		void PutAddress(Out& a_out, std::uintptr_t a_address)
		{
			if (const Module* m = FindModule(a_address)) {
				Put(a_out, "%s+%llX", m->name, static_cast<unsigned long long>(a_address - m->base));
				char           symbol[128];
				std::uintptr_t offset = 0;
				if (m->isSystem && ExportName(*m, a_address, symbol, sizeof(symbol), offset)) {
					Put(a_out, " (%s+0x%llX)", symbol, static_cast<unsigned long long>(offset));
				}
			} else {
				Put(a_out, "(no module)");
			}
		}

		// Collects the objects found near the crash, without duplicates.
		struct FoundObject
		{
			char name[128];
			char text[320];
			char source[128];
		};
		struct Classes
		{
			FoundObject items[32];
			int         count;
		};

		void AddObject(Classes& a_classes, const char* a_name, const char* a_text, const char* a_source)
		{
			for (int i = 0; i < a_classes.count; ++i) {
				if (strcmp(a_classes.items[i].name, a_name) == 0 && strcmp(a_classes.items[i].text, a_text) == 0) {
					return;
				}
			}
			if (a_classes.count < 32) {
				auto& item = a_classes.items[a_classes.count++];
				strncpy_s(item.name, a_name, _TRUNCATE);
				strncpy_s(item.text, a_text, _TRUNCATE);
				strncpy_s(item.source, a_source, _TRUNCATE);
			}
		}

		// Writes what a value points to. Returns true if it wrote something.
		bool Annotate(Out& a_out, std::uintptr_t a_value, Classes& a_classes)
		{
			if (const Module* m = FindModule(a_value)) {
				Put(a_out, "  %s+%llX", m->name, static_cast<unsigned long long>(a_value - m->base));
				return true;
			}
			static RttiInfo info;
			if (RttiLookup(a_value, info)) {
				static char text[320];
				static char source[128];
				Put(a_out, "  (%s*)", info.name);
				if (Describe(info, text, sizeof(text), source, sizeof(source))) {
					Put(a_out, " %s", text);
				}
				AddObject(a_classes, info.name, text, source);
				return true;
			}
			char text[49] = {};
			if (SafeRead(a_value, text, 48) || SafeRead(a_value, text, 16)) {
				int n = 0;
				while (n < 48 && text[n] >= 0x20 && text[n] < 0x7F) {
					++n;
				}
				if (n >= 5 && (text[n] == '\0' || n == 48)) {
					text[n] = '\0';
					Put(a_out, "  \"%s\"", text);
					return true;
				}
			}
			return false;
		}

		const char* ExceptionName(DWORD a_code)
		{
			switch (a_code) {
			case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
			case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
			case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
			case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
			case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
			case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
			case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
			case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
			case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
			case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
			case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
			case kCppException: return "C++ exception";
			case 0xC0000374: return "STATUS_HEAP_CORRUPTION";
			case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN (fail fast)";
			default: return "unknown";
			}
		}

		bool CallSection(SectionFunction a_function, char* a_out, std::size_t a_cap)
		{
			__try {
				a_function(a_out, a_cap);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return false;
		}

		// Writes the sections that other parts of the tool registered.
		void AppendSections(Out& a_out)
		{
			static char text[8192];
			const int   count = g_sectionCount.load();
			for (int i = 0; i < count; ++i) {
				text[0] = '\0';
				Put(a_out, "\n== %s ==\n", g_sections[i].title);
				if (CallSection(g_sections[i].function, text, sizeof(text) - 1)) {
					text[sizeof(text) - 1] = '\0';
					Put(a_out, "%s", text);
				} else {
					Put(a_out, "(not available: an error occurred while this section was made)\n");
				}
			}
		}

		// ---------------------------------------------------------- stack walk

		int WalkStack(CONTEXT a_context, std::uintptr_t* a_frames, int a_max)
		{
			int n = 0;
			__try {
				while (n < a_max && a_context.Rip != 0) {
					a_frames[n++] = a_context.Rip;

					DWORD64 imageBase = 0;
					const auto function = LookupFunction(a_context.Rip, imageBase);
					if (!function) {
						// Leaf function, or code with no unwind data: the
						// return address is at the top of the stack.
						DWORD64 returnAddress = 0;
						if (!SafeGet(a_context.Rsp, returnAddress) || !FindModule(returnAddress)) {
							break;
						}
						a_context.Rip = returnAddress;
						a_context.Rsp += 8;
					} else {
						void*   handlerData = nullptr;
						DWORD64 establisher = 0;
						RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, a_context.Rip, function, &a_context, &handlerData, &establisher, nullptr);
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return n;
		}

		// -------------------------------------------------------- C++ exception

		bool SafeWhat(std::uintptr_t a_object, char* a_out, std::size_t a_cap)
		{
			__try {
				const char* what = reinterpret_cast<const std::exception*>(a_object)->what();
				if (what) {
					strncpy_s(a_out, a_cap, what, _TRUNCATE);
					return true;
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return false;
		}

		struct CppInfo
		{
			bool valid;
			char type[128];
			bool hasMessage;
			char message[512];
		};

		void ReadCppException(const EXCEPTION_RECORD& a_record, CppInfo& a_info)
		{
			a_info.valid = false;
			a_info.hasMessage = false;
			if (a_record.ExceptionCode != kCppException || a_record.NumberParameters < 4) {
				return;
			}
			const auto object = static_cast<std::uintptr_t>(a_record.ExceptionInformation[1]);
			const auto throwInfo = static_cast<std::uintptr_t>(a_record.ExceptionInformation[2]);
			const auto imageBase = static_cast<std::uintptr_t>(a_record.ExceptionInformation[3]);

			std::int32_t arrayRva = 0;
			std::int32_t count = 0;
			if (!SafeGet(throwInfo + 0xC, arrayRva) || !SafeGet(imageBase + arrayRva, count) || count <= 0 || count > 64) {
				return;
			}
			for (std::int32_t i = 0; i < count; ++i) {
				std::int32_t typeRva = 0;
				std::int32_t descriptorRva = 0;
				std::int32_t thisOffset = 0;
				if (!SafeGet(imageBase + arrayRva + 4 + i * 4, typeRva) ||
					!SafeGet(imageBase + typeRva + 4, descriptorRva) ||
					!SafeGet(imageBase + typeRva + 8, thisOffset)) {
					return;
				}
				char name[128];
				if (!ReadTypeName(imageBase + descriptorRva, name, sizeof(name))) {
					return;
				}
				if (i == 0) {
					strncpy_s(a_info.type, name, _TRUNCATE);
					a_info.valid = true;
				}
				if (strcmp(name, "std::exception") == 0) {
					a_info.hasMessage = SafeWhat(object + thisOffset, a_info.message, sizeof(a_info.message));
					return;
				}
			}
		}

		// -------------------------------------------------------------- report

		void AppendCaught(Out& a_out, std::uintptr_t a_crashAddress);

		// The type of an access violation, in plain words. Uses the state of
		// the memory at the address.
		void DescribeAccess(Out& a_out, ULONG_PTR a_kind, std::uintptr_t a_target, char* a_what, std::size_t a_whatCap)
		{
			const char* op = a_kind == 1 ? "write to" : (a_kind == 8 ? "run code at" : "read from");
			const auto  target = static_cast<unsigned long long>(a_target);
			if (a_target == ~static_cast<std::uintptr_t>(0)) {
				// The processor does not give the address when a pointer is
				// not in the valid (canonical) address range. Windows then
				// reports address 0xFFFFFFFFFFFFFFFF.
				Put(a_out, "Code used a pointer with a value that cannot be an address. The pointer value is not available. "
						   "The usual cause is a corrupted pointer or an object that was deleted and then overwritten.\n");
				_snprintf_s(a_what, a_whatCap, _TRUNCATE, "The game crashed: code used a damaged pointer (its value cannot be an address).");
				return;
			}
			if (a_target < 0x10000) {
				if (a_target == 0) {
					Put(a_out, "Code used a null pointer: an object that does not exist (%s address 0).\n", op);
				} else {
					Put(a_out, "Code used the part at offset 0x%llX of an object that does not exist: a null pointer plus an offset (%s address 0x%llX).\n",
						target, op, target);
				}
				_snprintf_s(a_what, a_whatCap, _TRUNCATE, "The game crashed: code used an object that does not exist (a null pointer).");
				return;
			}
			MEMORY_BASIC_INFORMATION info{};
			const bool               known = VirtualQuery(reinterpret_cast<LPCVOID>(a_target), &info, sizeof(info)) == sizeof(info);
			const Module*            module = FindModule(a_target);
			if (a_kind == 8) {
				if (module) {
					Put(a_out, "The processor tried to run code at %s+%llX: in a module, but at a place that is not code. The usual cause is a damaged "
							   "function pointer.\n",
						module->name, static_cast<unsigned long long>(a_target - module->base));
					_snprintf_s(a_what, a_whatCap, _TRUNCATE, "The game crashed: code jumped to a place that is not code (a damaged function pointer).");
				} else if (known && info.State == MEM_FREE) {
					Put(a_out, "The processor tried to run code at address 0x%llX, which is free memory: the code that was there was removed. The usual "
							   "cause is a hook of a DLL that was unloaded, or a function of an object that was deleted.\n",
						target);
					_snprintf_s(a_what, a_whatCap, _TRUNCATE, "The game crashed: code jumped into memory that was freed (a removed hook or a deleted object).");
				} else {
					Put(a_out, "The processor tried to run code at address 0x%llX, where there is no code. The usual cause is a bad function pointer or a "
							   "deleted object.\n",
						target);
					_snprintf_s(a_what, a_whatCap, _TRUNCATE, "The game crashed: code jumped to an address with no code (a bad function pointer or a removed hook).");
				}
				return;
			}
			if (known && info.State == MEM_FREE) {
				Put(a_out, "Code used a pointer to memory that is free (%s address 0x%llX): the object was deleted, and code used it after that.\n", op, target);
				if (a_target < 0x100000000ull) {
					Put(a_out, "The value 0x%llX is small for a pointer. It can be a FormID (load order index %02llX) or a number that code used as a pointer "
							   "by mistake.\n",
						target, (target >> 24) & 0xFF);
				}
				_snprintf_s(a_what, a_whatCap, _TRUNCATE, "The game crashed: code used an object that was already deleted (the memory is free).");
			} else if (known && info.State == MEM_RESERVE) {
				Put(a_out, "Code used an address in memory that is reserved but not in use (%s address 0x%llX). The usual cause is an index or a size "
						   "past the end of a block of memory.\n",
					op, target);
				_snprintf_s(a_what, a_whatCap, _TRUNCATE, "The game crashed: code went past the end of a block of memory (a bad index or size).");
			} else if (known && info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) != 0) {
				Put(a_out, "Code touched a guard page (%s address 0x%llX), usually the end of a thread stack.\n", op, target);
				_snprintf_s(a_what, a_whatCap, _TRUNCATE, "The game crashed: code went past the end of a thread stack.");
			} else if (known && info.State == MEM_COMMIT && a_kind == 1 &&
					   (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) {
				Put(a_out, "Code tried to write to memory that is read-only (address 0x%llX%s%s).\n", target, module ? " in " : "", module ? module->name : "");
				_snprintf_s(a_what, a_whatCap, _TRUNCATE, "The game crashed: code tried to change memory that is read-only.");
			} else {
				Put(a_out, "Code used a pointer to memory that is not valid (%s address 0x%llX). The usual cause is a deleted or damaged object.\n", op, target);
				_snprintf_s(a_what, a_whatCap, _TRUNCATE, "The game crashed: code used a pointer that is not valid (a deleted or damaged object).");
			}
		}

		bool IsGraphicsDriver(const char* a_name)
		{
			static const char* const kPrefixes[] = { "nvwgf", "nvd3d", "nvldumd", "nvoglv", "amdx", "atidxx", "atiumd", "atiuxp", "aticfx", "igd10", "igd11",
				"igd12", "igc64", "igdumdim", "intelocl" };
			for (const char* p : kPrefixes) {
				if (_strnicmp(a_name, p, strlen(p)) == 0) {
					return true;
				}
			}
			return false;
		}

		void BuildReport(Out& a_out, const EXCEPTION_RECORD& a_record, const CONTEXT& a_context, DWORD a_threadId)
		{
			memset(&g_quick, 0, sizeof(g_quick));
			g_quick.confidence = "low";
			g_quick.closing = monitor::IsClosing();

			SYSTEMTIME now;
			GetLocalTime(&now);
			const ULONGLONG uptime = (GetTickCount64() - g_startTick) / 1000;

			Put(a_out, "SkyrimDiagnostics crash report\n");
			Put(a_out, "Tool version: %s\n", g_toolVersion);
			Put(a_out, "Game: %s\n", g_gameVersion);
			Put(a_out, "Platform: %s\n", g_platform);
			Put(a_out, "Time: %04u-%02u-%02u %02u:%02u:%02u (local)\n", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
			Put(a_out, "Session length: %llu min %llu s\n", uptime / 60, uptime % 60);
			if (g_note[0] != '\0') {
				Put(a_out, "Note: %s\n", g_note);
			}
			if (g_save[0] != '\0') {
				Put(a_out, "Save: %s\n", g_save);
			}
			if (g_setup[0] != '\0') {
				Put(a_out, "Mod setup: %s\n", g_setup);
			}

			const auto code = a_record.ExceptionCode;
			const auto address = reinterpret_cast<std::uintptr_t>(a_record.ExceptionAddress);

			const std::size_t exceptionStart = a_out.len;
			Put(a_out, "\n== EXCEPTION ==\n");
			Put(a_out, "Code: 0x%08lX %s\n", code, ExceptionName(code));
			Put(a_out, "Address: 0x%016llX ", static_cast<unsigned long long>(address));
			PutAddress(a_out, address);
			Put(a_out, "\nThread: %lu%s\n", a_threadId,
				g_mainThreadId == 0 ? "" : (a_threadId == g_mainThreadId ? " (main thread)" : " (not the main thread)"));

			bool           isAccess = false;
			std::uintptr_t accessTarget = 0;
			ULONG_PTR      accessKind = 0;
			if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) && a_record.NumberParameters >= 2) {
				isAccess = true;
				accessKind = a_record.ExceptionInformation[0];
				accessTarget = static_cast<std::uintptr_t>(a_record.ExceptionInformation[1]);
				Put(a_out, "Operation: %s address 0x%016llX\n",
					accessKind == 0 ? "read from" : (accessKind == 1 ? "write to" : "execute"),
					static_cast<unsigned long long>(accessTarget));
			}

			CppInfo cpp{};
			ReadCppException(a_record, cpp);
			if (cpp.valid) {
				Put(a_out, "C++ type: %s\n", cpp.type);
				if (cpp.hasMessage) {
					Put(a_out, "C++ message: %s\n", cpp.message);
				}
			}

			// Call stack.
			static std::uintptr_t frames[kMaxFrames];
			const int             frameCount = WalkStack(a_context, frames, kMaxFrames);

			const std::size_t callStart = a_out.len;
			Put(a_out, "\n== CALL STACK ==\n");
			for (int i = 0; i < frameCount; ++i) {
				Put(a_out, "#%-3d 0x%016llX  ", i, static_cast<unsigned long long>(frames[i]));
				PutAddress(a_out, frames[i]);
				Put(a_out, "\n");
			}
			if (frameCount == kMaxFrames) {
				Put(a_out, "(call stack cut at %d frames)\n", kMaxFrames);
			}

			// Registers.
			static Classes classes;
			classes.count = 0;

			Put(a_out, "\n== REGISTERS ==\n");
			const struct
			{
				const char* name;
				DWORD64     value;
			} registers[] = {
				{ "RAX", a_context.Rax }, { "RBX", a_context.Rbx }, { "RCX", a_context.Rcx }, { "RDX", a_context.Rdx },
				{ "RSI", a_context.Rsi }, { "RDI", a_context.Rdi }, { "RBP", a_context.Rbp }, { "RSP", a_context.Rsp },
				{ "R8 ", a_context.R8 },  { "R9 ", a_context.R9 },  { "R10", a_context.R10 }, { "R11", a_context.R11 },
				{ "R12", a_context.R12 }, { "R13", a_context.R13 }, { "R14", a_context.R14 }, { "R15", a_context.R15 },
				{ "RIP", a_context.Rip },
			};
			for (const auto& reg : registers) {
				Put(a_out, "%s 0x%016llX", reg.name, static_cast<unsigned long long>(reg.value));
				Annotate(a_out, static_cast<std::uintptr_t>(reg.value), classes);
				Put(a_out, "\n");
			}

			// Stack values that point to something known.
			const std::size_t stackStart = a_out.len;
			Put(a_out, "\n== STACK (values that point to code, objects or text) ==\n");
			for (int i = 0; i < kStackSlots; ++i) {
				const std::uintptr_t slot = static_cast<std::uintptr_t>(a_context.Rsp) + static_cast<std::uintptr_t>(i) * 8;
				std::uintptr_t       value = 0;
				if (!SafeGet(slot, value)) {
					break;
				}
				const std::size_t mark = a_out.len;
				Put(a_out, "[RSP+%03X] 0x%016llX", i * 8, static_cast<unsigned long long>(value));
				if (Annotate(a_out, value, classes)) {
					Put(a_out, "\n");
				} else {
					a_out.len = mark;  // not useful: remove the line
					a_out.buf[mark] = '\0';
				}
			}

			// Modules.
			const auto&       table = Modules();
			const std::size_t modulesStart = a_out.len;
			Put(a_out, "\n== MODULES (%d) ==\n", table.count);
			for (int i = 0; i < table.count; ++i) {
				const auto& m = table.items[i];
				const char* mod = ModOf(m);
				Put(a_out, "0x%016llX  %-48s%s%s%s\n", static_cast<unsigned long long>(m.base), m.name, ModuleTag(m), mod[0] ? "  mod: " : "", mod);
			}

			hooks::AppendSummary(a_out);
			profiler::Append(a_out);
			memtrack::Append(a_out);
			AppendSections(a_out);
			session::AppendRecent(a_out);

			// Probable cause. It is the last section of the text; WriteReportFile
			// puts it right after the quick answer in the file.
			Put(a_out, "\n== PROBABLE CAUSE ==\n");
			const bool memoryFull = session::AppendMemoryWarning(a_out);
			if (g_quick.closing) {
				Put(a_out, "The player had chosen to quit or to close the window before this crash: it is a crash while the game closes, not a crash in "
						   "play. Some DLLs run code while the game unloads them. It is usually harmless, but report it to the author of the DLL named below.\n");
			}

			const Module* crashModule = frameCount > 0 ? FindModule(frames[0]) : nullptr;
			if (crashModule) {
				_snprintf_s(g_quick.group, _TRUNCATE, "%s+%llX %08lX", crashModule->name, static_cast<unsigned long long>(frames[0] - crashModule->base), code);
			} else {
				_snprintf_s(g_quick.group, _TRUNCATE, "no-module %08lX", code);
			}
			const Module* firstMod = nullptr;
			int           firstModFrame = -1;
			for (int i = 0; i < frameCount; ++i) {
				const Module* m = FindModule(frames[i]);
				if (IsModLibrary(m)) {
					firstMod = m;
					firstModFrame = i;
					break;
				}
			}

			if (code == EXCEPTION_STACK_OVERFLOW) {
				Put(a_out, "The thread used all of its stack. The usual cause is a function that calls itself without end.\n");
				_snprintf_s(g_quick.what, _TRUNCATE, "The game crashed: a thread used all of its stack (usually a function that calls itself without end).");
			} else if (cpp.valid) {
				Put(a_out, "Code threw a C++ exception of type %s and no code caught it.\n", cpp.type);
				if (cpp.hasMessage) {
					Put(a_out, "The exception message is: %s\n", cpp.message);
				}
				_snprintf_s(g_quick.what, _TRUNCATE, "The game crashed: a C++ exception (%s%s%s) that no code caught.", cpp.type, cpp.hasMessage ? ": " : "",
					cpp.hasMessage ? cpp.message : "");
			} else if (isAccess) {
				DescribeAccess(a_out, accessKind, accessTarget, g_quick.what, sizeof(g_quick.what));
				if (accessKind == 8 && hooks::AppendTo(a_out, accessTarget) > 0) {
					strncat_s(g_quick.what, " A hook of a DLL went to that place (see below).", _TRUNCATE);
				}
			} else {
				_snprintf_s(g_quick.what, _TRUNCATE, "The game crashed: exception 0x%08lX (%s).", code, ExceptionName(code));
			}

			if (firstMod && firstModFrame == 0) {
				Put(a_out, "Most probable source: %s. The crash occurred in the code of this DLL.\n", NameWithMod(*firstMod));
				Put(a_out, "Confidence: high.\n");
				strncpy_s(g_quick.suspect, NameWithMod(*firstMod), _TRUNCATE);
				g_quick.confidence = "high";
				_snprintf_s(g_quick.tryText, _TRUNCATE, "Update %s. If the crash comes again at the same place, disable it for a test.", firstMod->name);
			} else if (firstMod) {
				Put(a_out, "Most probable source: %s. This DLL is the nearest mod DLL on the call stack (frame #%d). It called the code that crashed",
					NameWithMod(*firstMod), firstModFrame);
				if (crashModule) {
					Put(a_out, " in %s", crashModule->name);
				}
				Put(a_out, ".\nConfidence: medium. The DLL can be the cause, or it can be a hook that only passed bad data along.\n");
				strncpy_s(g_quick.suspect, NameWithMod(*firstMod), _TRUNCATE);
				g_quick.confidence = "medium";
				_snprintf_s(g_quick.tryText, _TRUNCATE, "Update %s. If the crash comes again at the same place, disable it for a test.", firstMod->name);
			} else if (crashModule && crashModule->isSelf) {
				Put(a_out, "The crash occurred in %s, which is this diagnostics tool, and no mod DLL is on the call stack.\n", crashModule->name);
				Put(a_out, "If this was not a test crash, it is a defect in the tool. Report it with this file.\n");
				Put(a_out, "Confidence: high.\n");
				_snprintf_s(g_quick.suspect, _TRUNCATE, "%s (this diagnostics tool)", crashModule->name);
				g_quick.confidence = "high";
				_snprintf_s(g_quick.tryText, _TRUNCATE, "If this was not a test crash, report it to the author of SkyrimDiagnostics with this file.");
			} else if (crashModule && crashModule->isGame) {
				Put(a_out, "The crash occurred in the game executable and no mod DLL is on the call stack.\n");
				Put(a_out, "The usual cause is bad game data: a mesh, a texture, a plugin record or a save file. No DLL can be named.\n");
				Put(a_out, "Confidence: low.\n");
				_snprintf_s(g_quick.suspect, _TRUNCATE, "no mod DLL (the crash is in the game code)");
			} else if (crashModule) {
				Put(a_out, "The crash occurred in %s and no mod DLL is on the call stack.\n", crashModule->name);
				Put(a_out, "Confidence: low.\n");
				strncpy_s(g_quick.suspect, crashModule->name, _TRUNCATE);
				if (IsGraphicsDriver(crashModule->name)) {
					_snprintf_s(g_quick.tryText, _TRUNCATE,
						"%s is part of the graphics driver. Update the graphics driver. If you use ENB, ReShade or an overlay, turn it off for a test.",
						crashModule->name);
				}
			} else {
				Put(a_out, "The crash address is not in a loaded module and no mod DLL is on the call stack.\n");
				Put(a_out, "Confidence: low.\n");
			}

			if (crashModule && crashModule->isGame) {
				char      head[] = "\nHooks of DLLs near the crash place in the game code:\n";
				const std::size_t mark = a_out.len;
				Put(a_out, "%s", head);
				if (hooks::AppendNear(a_out, frames[0]) == 0) {
					a_out.len = mark;
					a_out.buf[mark] = '\0';
				}
			}
			AppendCaught(a_out, address);

			if (classes.count > 0) {
				Put(a_out, "\nObjects found near the crash (registers first, then stack):\n");
				for (int i = 0; i < classes.count; ++i) {
					Put(a_out, "  %s%s%s\n", classes.items[i].name, classes.items[i].text[0] ? ": " : "", classes.items[i].text);
				}

				// Files that the objects come from, with the number of objects.
				static char sources[32][128];
				static int  sourceHits[32];
				int         sourceCount = 0;
				for (int i = 0; i < classes.count; ++i) {
					const char* source = classes.items[i].source;
					if (source[0] == '\0') {
						continue;
					}
					int found = -1;
					for (int k = 0; k < sourceCount; ++k) {
						if (_stricmp(sources[k], source) == 0) {
							found = k;
							break;
						}
					}
					if (found < 0) {
						found = sourceCount++;
						strncpy_s(sources[found], source, _TRUNCATE);
						sourceHits[found] = 0;
					}
					++sourceHits[found];
				}
				if (sourceCount > 0 && g_quick.tryText[0] == '\0') {
					_snprintf_s(g_quick.tryText, _TRUNCATE,
						"The objects near the crash come mostly from %s. Look at that plugin first: update it, or disable it for a test with a new save.", sources[0]);
				}
				if (sourceCount > 0) {
					Put(a_out, "\nFiles that these objects come from (nearest to the crash first):\n");
					for (int k = 0; k < sourceCount; ++k) {
						Put(a_out, "  %s (%d object%s)\n", sources[k], sourceHits[k], sourceHits[k] == 1 ? "" : "s");
					}
					if (!firstMod && !(crashModule && crashModule->isSelf)) {
						Put(a_out, "No mod DLL is on the call stack, thus the first file in this list is the best available indication. "
								   "An object can be near the crash by chance: treat this as a place to start, not as proof.\n");
					}
				}
			}

			// Known crash signs (Phostwood's Crash Log Analyzer data). The report text up to
			// the MODULES section is the text that is searched.
			{
				static const char* loaded[kMaxModules];
				static const char* onStack[kMaxFrames];
				int                onStackCount = 0;
				for (int i = 0; i < table.count; ++i) {
					loaded[i] = table.items[i].name;
				}
				for (int i = 0; i < frameCount; ++i) {
					const Module* m = FindModule(frames[i]);
					if (!m) {
						continue;
					}
					bool seen = false;
					for (int k = 0; k < onStackCount && !seen; ++k) {
						seen = onStack[k] == m->name;
					}
					if (!seen) {
						onStack[onStackCount++] = m->name;
					}
				}
				char gameOffset[32] = "";
				if (crashModule && crashModule->isGame) {
					_snprintf_s(gameOffset, _TRUNCATE, "%llX", static_cast<unsigned long long>(frames[0] - crashModule->base));
				}
				phostwood::CrashInput in;
				in.exception = { a_out.buf + exceptionStart, callStart - exceptionStart };
				in.callStackAndRegs = { a_out.buf + callStart, stackStart - callStart };
				in.top = { a_out.buf + callStart, modulesStart - callStart };
				in.gameOffset = gameOffset;
				in.loaded = loaded;
				in.loadedCount = table.count;
				in.onStack = onStack;
				in.onStackCount = onStackCount;
				const int found = phostwood::Append(a_out, in);
				if ((found & phostwood::kFoundPrograms) != 0 && g_quick.confidence[0] == 'l' && g_quick.tryText[0] == '\0') {
					_snprintf_s(g_quick.tryText, _TRUNCATE,
						"An antivirus or overlay program has code in the game (see the end of this report). Add the game folder as an exception, or turn it off for a test.");
				}
			}
			if (memoryFull) {
				_snprintf_s(g_quick.tryText, _TRUNCATE, "Memory was almost full at the crash. Close other programs, check the Windows page file, and use lower "
														 "texture sizes. Then test again.");
			}
			if (g_quick.tryText[0] == '\0') {
				_snprintf_s(g_quick.tryText, _TRUNCATE, "No mod can be named. Read PROBABLE CAUSE below, and note what you did when the game crashed.");
			}
		}

		// Writes a report: its head lines (up to the first section), then a_quick
		// (or nothing), then the PROBABLE CAUSE section (to the end of the text),
		// then the other sections. The text is made with PROBABLE CAUSE last.
		// No heap memory.
		bool WriteReportFile(const wchar_t* a_path, const Out& a_out, const Out* a_quick)
		{
			const char*       body = a_out.buf;
			const std::size_t length = a_out.len;
			const char*       firstSection = strstr(body, "\n== ");
			const std::size_t headLen = firstSection ? static_cast<std::size_t>(firstSection - body) : length;
			const char*       cause = strstr(body, "\n== PROBABLE CAUSE ==");
			const std::size_t causeAt = cause ? static_cast<std::size_t>(cause - body) : length;

			const HANDLE file = CreateFileW(a_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return false;
			}
			DWORD written = 0;
			bool  ok = WriteFile(file, body, static_cast<DWORD>(headLen), &written, nullptr) != FALSE;
			if (a_quick) {
				ok = WriteFile(file, a_quick->buf, static_cast<DWORD>(a_quick->len), &written, nullptr) != FALSE && ok;
			}
			if (causeAt < length) {
				ok = WriteFile(file, body + causeAt, static_cast<DWORD>(length - causeAt), &written, nullptr) != FALSE && ok;
			}
			if (causeAt > headLen) {
				ok = WriteFile(file, body + headLen, static_cast<DWORD>(causeAt - headLen), &written, nullptr) != FALSE && ok;
			}
			FlushFileBuffers(file);
			CloseHandle(file);
			return ok;
		}

		// The crash history: one line for each crash, in crash-history.txt in
		// the report folder. Returns the number of earlier crashes at the same
		// place, and the time of the last one. No heap memory.
		int CrashHistory(const char* a_group, const SYSTEMTIME& a_now, bool a_add, char* a_last, std::size_t a_lastCap)
		{
			a_last[0] = '\0';
			if (a_group[0] == '\0') {
				return 0;
			}
			wchar_t path[800];
			swprintf_s(path, L"%s\\crash-history.txt", g_reportDir);
			static char text[512 * 1024];
			DWORD       read = 0;
			const HANDLE in = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (in != INVALID_HANDLE_VALUE) {
				ReadFile(in, text, sizeof(text) - 1, &read, nullptr);
				CloseHandle(in);
			}
			text[read] = '\0';
			// Lines: "YYYY-MM-DD HH:MM:SS<tab>group<tab>suspect"
			int         count = 0;
			const char* line = text;
			const std::size_t groupLen = strlen(a_group);
			while (*line) {
				const char* end = strchr(line, '\n');
				const char* tab = strchr(line, '\t');
				if (tab && (!end || tab < end) && strncmp(tab + 1, a_group, groupLen) == 0 && tab[1 + groupLen] == '\t') {
					++count;
					const std::size_t n = static_cast<std::size_t>(tab - line);
					_snprintf_s(a_last, a_lastCap, _TRUNCATE, "%.*s", static_cast<int>(n), line);
				}
				if (!end) {
					break;
				}
				line = end + 1;
			}
			if (a_add) {
				const HANDLE out = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (out != INVALID_HANDLE_VALUE) {
					char      entry[700];
					const int n = _snprintf_s(entry, _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u\t%s\t%s\n", a_now.wYear, a_now.wMonth, a_now.wDay, a_now.wHour,
						a_now.wMinute, a_now.wSecond, a_group, g_quick.suspect);
					DWORD written = 0;
					WriteFile(out, entry, static_cast<DWORD>(n > 0 ? n : 0), &written, nullptr);
					CloseHandle(out);
				}
			}
			return count;
		}

		// The short answer, at the top of the file.
		void PutQuick(Out& a_out, int a_repeats, const char* a_last)
		{
			Put(a_out, "\n== QUICK ANSWER ==\n");
			if (g_quick.closing) {
				Put(a_out, "What happened: the game crashed while it was closing (you had chosen to quit or to close the window). This is not a crash in "
						   "play. %s\n",
					g_quick.what);
			} else {
				Put(a_out, "What happened: %s\n", g_quick.what);
			}
			Put(a_out, "Suspect: %s. Likelihood: %s.\n", g_quick.suspect[0] ? g_quick.suspect : "none can be named", g_quick.confidence);
			Put(a_out, "Try: %s\n", g_quick.tryText);
			if (a_repeats > 0) {
				Put(a_out, "Repeat: a crash at the same place (%s) happened %d time%s before, the last on %s.\n", g_quick.group, a_repeats,
					a_repeats == 1 ? "" : "s", a_last);
			}
			Put(a_out, "PROBABLE CAUSE follows; the details are below it.\n");
		}

		void WriteFiles(Out& a_out, EXCEPTION_POINTERS* a_pointers, DWORD a_threadId)
		{
			SYSTEMTIME now;
			GetLocalTime(&now);

			wchar_t path[800];
			swprintf_s(path, L"%s\\crash-%04u-%02u-%02u-%02u-%02u-%02u.log", g_reportDir, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
				now.wSecond);

			bool dumpWritten = false;
			bool dumpByMonitor = false;
			if (g_writeMinidump) {
				wchar_t dumpPath[800];
				swprintf_s(dumpPath, L"%s\\crash-%04u-%02u-%02u-%02u-%02u-%02u.dmp", g_reportDir, now.wYear, now.wMonth, now.wDay, now.wHour,
					now.wMinute, now.wSecond);
				// First from outside the game (the monitor process), else from here.
				dumpByMonitor = monitor::RequestDump(dumpPath, a_pointers, a_threadId, 30000);
				dumpWritten = dumpByMonitor;
				const HANDLE dump = dumpWritten ? INVALID_HANDLE_VALUE : CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (dump != INVALID_HANDLE_VALUE) {
					MINIDUMP_EXCEPTION_INFORMATION info{};
					info.ThreadId = a_threadId;
					info.ExceptionPointers = a_pointers;
					info.ClientPointers = FALSE;
					__try {
						dumpWritten = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
										  static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules), &info,
										  nullptr, nullptr) != FALSE;
					} __except (EXCEPTION_EXECUTE_HANDLER) {
						dumpWritten = false;
					}
					CloseHandle(dump);
					if (!dumpWritten) {
						DeleteFileW(dumpPath);
					}
				}
			}
			Put(a_out, "\nMinidump: %s\n", !g_writeMinidump ? "disabled" :
											   (dumpWritten ? (dumpByMonitor ? "written by the monitor process (same file name, .dmp)" : "written (same file name, .dmp)") :
															  "not written (error)"));

			// The crash history: a crash while the game closes is not counted.
			char      last[64];
			const int repeats = CrashHistory(g_quick.group, now, !g_quick.closing, last, sizeof(last));

			// The file: the head lines, the short answer, PROBABLE CAUSE, then the details.
			static char quick[4096];
			Out         q{ quick, sizeof(quick), 0 };
			quick[0] = '\0';
			PutQuick(q, repeats, last);
			WriteReportFile(path, a_out, &q);
			FILETIME crashTime{};
			GetSystemTimeAsFileTime(&crashTime);
			monitor::NoteReport(path, g_openReport && !g_quick.closing, (static_cast<std::uint64_t>(crashTime.dwHighDateTime) << 32) | crashTime.dwLowDateTime);
		}

		// The report is made on this thread, not on the crashed thread. Thus
		// a crashed thread with no stack space left (stack overflow) is not a
		// problem.
		DWORD WINAPI ReporterThread(LPVOID)
		{
			for (;;) {
				WaitForSingleObject(g_goEvent, INFINITE);

				EXCEPTION_POINTERS* pointers = g_crashPointers;
				const DWORD         threadId = g_crashThreadId;

				Out out{ g_report, kReportCap, 0 };
				g_report[0] = '\0';
				__try {
					BuildReport(out, *pointers->ExceptionRecord, *pointers->ContextRecord, threadId);
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					Put(out, "\n(The report is not complete: an error occurred while it was made.)\n");
				}
				WriteFiles(out, pointers, threadId);
				session::NoteCrashReported();

				SetEvent(g_doneEvent);
			}
		}

		LONG CALLBACK CountException(EXCEPTION_POINTERS* a_pointers)
		{
			const DWORD code = a_pointers->ExceptionRecord->ExceptionCode;
			if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
				code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			if (g_inCrash || g_inStall) {
				return EXCEPTION_CONTINUE_SEARCH;  // the report code reads memory that can be bad
			}
			const LONG index = InterlockedIncrement(&g_caughtCount) - 1;
			Caught&    c = g_caught[index % kCaughtRing];
			c.tick = GetTickCount64();
			c.address = reinterpret_cast<std::uintptr_t>(a_pointers->ExceptionRecord->ExceptionAddress);
			c.code = code;
			c.threadId = GetCurrentThreadId();
			return EXCEPTION_CONTINUE_SEARCH;
		}

		// The exceptions that code caught before this crash (the last one is
		// the crash itself if it is at a_crashAddress).
		void AppendCaught(Out& a_out, std::uintptr_t a_crashAddress)
		{
			LONG total = g_caughtCount;
			if (total > 0 && g_caught[(total - 1) % kCaughtRing].address == a_crashAddress) {
				--total;  // the crash itself
			}
			// Modules that hide crashes on purpose, and other crash handlers.
			const auto& table = Modules();
			for (int i = 0; i < table.count; ++i) {
				char lower[96];
				strncpy_s(lower, table.items[i].name, _TRUNCATE);
				_strlwr_s(lower);
				if (strstr(lower, "crashguard") != nullptr) {
					Put(a_out, "\n%s is loaded. It hides crashes: the game goes on after an error, and the real cause shows later at a different place. "
							   "Disable it while you look for the cause of crashes.\n",
						table.items[i].name);
				}
			}
			const auto previous = reinterpret_cast<std::uintptr_t>(g_previousFilter.load());
			if (previous) {
				// The game's own filter is not a crash handler of a mod.
				if (const Module* m = FindModule(previous); m && !m->isSystem && !m->isGame) {
					Put(a_out, "\nA different crash handler is installed: %s. It runs after this tool.\n", m->name);
				}
			}
			if (total <= 0) {
				return;
			}
			Put(a_out, "\nErrors that code caught before this crash (the game went on after them): %ld. The last ones:\n", static_cast<long>(total));
			const ULONGLONG now = GetTickCount64();
			const LONG      first = total > 8 ? total - 8 : 0;
			for (LONG i = first; i < total; ++i) {
				const Caught& c = g_caught[i % kCaughtRing];
				Put(a_out, "  %llu s before the crash: 0x%08lX %s at ", (now - c.tick) / 1000, c.code, ExceptionName(c.code));
				PutAddress(a_out, c.address);
				Put(a_out, ", thread %lu\n", c.threadId);
			}
			Put(a_out, "Code that catches such errors and goes on can hide the real cause of a crash. If the same DLL is named above many times, "
					   "update it or disable it for a test.\n");
		}

		LONG WINAPI Filter(EXCEPTION_POINTERS* a_pointers)
		{
			if (InterlockedCompareExchange(&g_inCrash, 1, 0) == 0) {
				g_crashPointers = a_pointers;
				g_crashThreadId = GetCurrentThreadId();
				SetEvent(g_goEvent);
				WaitForSingleObject(g_doneEvent, 60000);
			} else {
				// A second thread crashed while the first report is in work.
				WaitForSingleObject(g_doneEvent, 60000);
			}

			const auto previous = g_previousFilter.load();
			if (previous && previous != &Filter) {
				return previous(a_pointers);
			}
			return EXCEPTION_CONTINUE_SEARCH;
		}

		void AssertFilter()
		{
			const auto previous = SetUnhandledExceptionFilter(&Filter);
			if (previous != &Filter) {
				// Other code replaced the filter. This handler goes first
				// again and calls the other filter after the report.
				g_previousFilter.store(previous);
			}
		}

		// Keeps a list of thread IDs. The list is made while the process is
		// healthy, because the snapshot function uses the heap and a frozen
		// thread can hold the heap lock.
		SRWLOCK g_threadRefreshLock = SRWLOCK_INIT;

		void RefreshThreads()
		{
			const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snapshot == INVALID_HANDLE_VALUE) {
				return;
			}
			// The maintenance thread and the profiler thread call this.
			AcquireSRWLockExclusive(&g_threadRefreshLock);
			const int   inactive = 1 - g_activeThreadTable.load();
			auto&       table = g_threadTables[inactive];
			const DWORD pid = GetCurrentProcessId();
			table.count = 0;

			THREADENTRY32 entry{};
			entry.dwSize = sizeof(entry);
			if (Thread32First(snapshot, &entry)) {
				do {
					if (entry.th32OwnerProcessID == pid && table.count < kMaxThreads) {
						table.ids[table.count++] = entry.th32ThreadID;
					}
					entry.dwSize = sizeof(entry);
				} while (Thread32Next(snapshot, &entry));
			}
			CloseHandle(snapshot);
			g_activeThreadTable.store(inactive, std::memory_order_release);
			ReleaseSRWLockExclusive(&g_threadRefreshLock);
		}

		DWORD WINAPI MaintenanceThread(LPVOID)
		{
			for (;;) {
				Sleep(5000);
				if (g_inCrash) {
					return 0;
				}
				if (g_inStall) {
					continue;
				}
				RefreshModules();
				RefreshThreads();
				AssertFilter();
			}
		}

		ULONGLONG ThreadCpu100ns(HANDLE a_thread)
		{
			FILETIME created, exited, kernel, user;
			if (!GetThreadTimes(a_thread, &created, &exited, &kernel, &user)) {
				return 0;
			}
			const auto toInt = [](const FILETIME& a_time) {
				return (static_cast<ULONGLONG>(a_time.dwHighDateTime) << 32) | a_time.dwLowDateTime;
			};
			return toInt(kernel) + toInt(user);
		}

		struct StallThread
		{
			DWORD          id;
			HANDLE         handle;
			ULONGLONG      cpuStart;
			int            cpuPercent;
			int            frameCount;
			std::uintptr_t frames[kMaxFrames];
		};

		void BuildStallReport(Out& a_out, DWORD a_focusThreadId, DWORD a_stallSeconds, StallKind a_kind)
		{
			constexpr int   kMaxStallThreads = 256;
			constexpr DWORD kSampleMs = 250;
			static StallThread threads[kMaxStallThreads];

			SYSTEMTIME now;
			GetLocalTime(&now);
			const ULONGLONG uptime = (GetTickCount64() - g_startTick) / 1000;

			// This first output also makes sure that the C runtime data of
			// this thread exists before other threads are suspended.
			Put(a_out, a_kind == StallKind::kLoadingScreen ? "SkyrimDiagnostics infinite loading screen report\n" : "SkyrimDiagnostics stall report\n");
			Put(a_out, "Tool version: %s\n", g_toolVersion);
			Put(a_out, "Game: %s\n", g_gameVersion);
			Put(a_out, "Platform: %s\n", g_platform);
			Put(a_out, "Time: %04u-%02u-%02u %02u:%02u:%02u (local)\n", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
			Put(a_out, "Session length: %llu min %llu s\n", uptime / 60, uptime % 60);
			if (g_save[0] != '\0') {
				Put(a_out, "Save: %s\n", g_save);
			}
			if (g_setup[0] != '\0') {
				Put(a_out, "Mod setup: %s\n", g_setup);
			}
			if (a_kind == StallKind::kLoadingScreen) {
				Put(a_out, "\nA loading screen is open for %lu s, and the game read no data from the disk in the last 30 s.\n", a_stallSeconds);
				Put(a_out, "The load does not go on: this is an infinite loading screen.\n");
			} else {
				Put(a_out, "\nThe game update thread (%lu) did not respond for %lu s.\n", a_focusThreadId, a_stallSeconds);
				Put(a_out, "This can be a freeze. It can also be a very long load.\n");
			}

			// Thread list: the focus thread first.
			const auto& table = g_threadTables[g_activeThreadTable.load(std::memory_order_acquire)];
			const DWORD self = GetCurrentThreadId();
			int         count = 0;
			const auto  add = [&](DWORD a_id) {
				if (a_id == self || a_id == 0 || count >= kMaxStallThreads) {
					return;
				}
				const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, a_id);
				if (!handle) {
					return;
				}
				auto& t = threads[count++];
				t.id = a_id;
				t.handle = handle;
				t.cpuStart = ThreadCpu100ns(handle);
				t.cpuPercent = 0;
				t.frameCount = 0;
			};
			add(a_focusThreadId);
			for (int i = 0; i < table.count; ++i) {
				if (table.ids[i] != a_focusThreadId) {
					add(table.ids[i]);
				}
			}

			// Processor use of each thread during a short sample.
			Sleep(kSampleMs);
			for (int i = 0; i < count; ++i) {
				const ULONGLONG used = ThreadCpu100ns(threads[i].handle) - threads[i].cpuStart;
				threads[i].cpuPercent = static_cast<int>(used / 100 / kSampleMs);  // 100 ns units to percent of the sample
				if (threads[i].cpuPercent > 100) {
					threads[i].cpuPercent = 100;
				}
			}

			// Call stacks. While a thread is suspended, this code only reads
			// memory and makes system calls. It does not print or allocate.
			for (int i = 0; i < count; ++i) {
				auto& t = threads[i];
				if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) {
					continue;
				}
				CONTEXT context{};
				context.ContextFlags = CONTEXT_FULL;
				if (GetThreadContext(t.handle, &context)) {
					t.frameCount = WalkStack(context, t.frames, i == 0 ? kMaxFrames : 24);
				}
				ResumeThread(t.handle);
			}

			for (int i = 0; i < count; ++i) {
				const auto& t = threads[i];
				Put(a_out, "\n== THREAD %lu%s, processor use %d%% ==\n", t.id, t.id == a_focusThreadId ? " (game update thread)" : "", t.cpuPercent);
				for (int f = 0; f < t.frameCount; ++f) {
					Put(a_out, "#%-3d 0x%016llX  ", f, static_cast<unsigned long long>(t.frames[f]));
					PutAddress(a_out, t.frames[f]);
					Put(a_out, "\n");
				}
				CloseHandle(t.handle);
			}

			const auto& modules = Modules();
			Put(a_out, "\n== MODULES (%d) ==\n", modules.count);
			for (int i = 0; i < modules.count; ++i) {
				const auto& m = modules.items[i];
				const char* mod = ModOf(m);
				Put(a_out, "0x%016llX  %-48s%s%s%s\n", static_cast<unsigned long long>(m.base), m.name, ModuleTag(m), mod[0] ? "  mod: " : "", mod);
			}

			profiler::Append(a_out);
			memtrack::Append(a_out);
			AppendSections(a_out);
			session::AppendRecent(a_out);

			Put(a_out, "\n== PROBABLE CAUSE ==\n");
			session::AppendMemoryWarning(a_out);
			if (count == 0 || threads[0].id != a_focusThreadId || threads[0].frameCount == 0) {
				Put(a_out, "The call stack of the game update thread is not available. No cause can be named.\n");
				return;
			}
			const auto& focus = threads[0];

			// What the game update thread waits for: an error dialog, the
			// graphics driver or the GPU, or a lock that a different thread holds.
			Put(a_out, "\nWait analysis of the game update thread:\n");
			const int dialogs = monitor::AppendDialogs(a_out, GetCurrentProcessId());
			std::uint32_t owner = 0;
			monitor::AppendWaitChain(a_out, focus.id, &owner);
			const Module* driver = nullptr;
			bool          present = false;
			for (int i = 0; i < focus.frameCount && i < 12; ++i) {
				const Module* m = FindModule(focus.frames[i]);
				if (!m) {
					continue;
				}
				if (!driver && IsGraphicsDriver(m->name)) {
					driver = m;
				}
				present = present || _stricmp(m->name, "dxgi.dll") == 0 || _stricmp(m->name, "d3d11.dll") == 0;
			}
			if (dialogs > 0) {
				Put(a_out, "A dialog window of the game is open (above). A DLL showed an error message and the game waits for it. The message usually "
						   "names the DLL or the problem.\nConfidence: high.\n\n");
			} else if (driver && focus.cpuPercent < 50) {
				Put(a_out, "The game update thread waits inside the graphics driver (%s). The usual causes: the GPU is busy or stopped (driver "
						   "timeout), a driver problem, or ENB, ReShade or an overlay.\n\n",
					driver->name);
			} else if (present && focus.cpuPercent < 50) {
				Put(a_out, "The game update thread waits in Direct3D (dxgi.dll or d3d11.dll): it waits for the GPU to show a frame.\n\n");
			}
			if (owner != 0) {
				for (int i = 0; i < count; ++i) {
					if (threads[i].id != owner) {
						continue;
					}
					const Module* ownerMod = nullptr;
					for (int f = 0; f < threads[i].frameCount && !ownerMod; ++f) {
						const Module* m = FindModule(threads[i].frames[f]);
						if (IsModLibrary(m)) {
							ownerMod = m;
						}
					}
					Put(a_out, "The game update thread waits for thread %lu (call stack above)%s%s.\n\n", static_cast<unsigned long>(owner),
						ownerMod ? ", which is in the code of " : "", ownerMod ? NameWithMod(*ownerMod) : "");
				}
			}

			if (focus.cpuPercent >= 50) {
				Put(a_out, "The game update thread is running code (processor use %d%%). It is in a loop that does not end, or in a very long operation.\n",
					focus.cpuPercent);
			} else {
				Put(a_out, "The game update thread is waiting (processor use %d%%). It waits for a lock, for a different thread, or for a file or device.\n",
					focus.cpuPercent);
			}
			const Module* top = FindModule(focus.frames[0]);
			const Module* firstMod = nullptr;
			int           firstModFrame = -1;
			bool          selfOnStack = false;
			for (int i = 0; i < focus.frameCount; ++i) {
				const Module* m = FindModule(focus.frames[i]);
				selfOnStack = selfOnStack || (m && m->isSelf);
				if (IsModLibrary(m)) {
					firstMod = m;
					firstModFrame = i;
					break;
				}
			}
			if (firstMod) {
				Put(a_out, "Most probable source: %s. It is the nearest mod DLL on the call stack of the game update thread (frame #%d).\n",
					NameWithMod(*firstMod), firstModFrame);
				Put(a_out, "Confidence: %s.\n", firstModFrame <= 3 ? "high" : "medium");
			} else if (selfOnStack) {
				Put(a_out, "The game update thread is stopped in code of this diagnostics tool and no mod DLL is on its call stack.\n");
				Put(a_out, "If this was not a test freeze, it is a defect in the tool. Report it with this file.\n");
				Put(a_out, "Confidence: high.\n");
			} else {
				Put(a_out, "No mod DLL is on the call stack of the game update thread. The thread is in %s.\n", top ? top->name : "code outside all modules");
				Put(a_out, "Look at the other threads with high processor use or with a mod DLL on their call stack.\n");
				Put(a_out, "Confidence: low.\n");
			}
		}
	}

	bool RegisterDescriber(const char* a_baseClass, Describer a_describer)
	{
		const int index = g_describerCount.load();
		if (!a_baseClass || !a_describer || index >= static_cast<int>(std::size(g_describers))) {
			return false;
		}
		strncpy_s(g_describers[index].baseClass, a_baseClass, _TRUNCATE);
		g_describers[index].function = a_describer;
		g_describerCount.store(index + 1);
		return true;
	}

	bool ClassName(const void* a_object, char* a_out, std::size_t a_cap)
	{
		RttiInfo info{};
		if (!a_object || !RttiLookup(reinterpret_cast<std::uintptr_t>(a_object), info)) {
			return false;
		}
		strncpy_s(a_out, a_cap, info.name, _TRUNCATE);
		return true;
	}

	bool RegisterSection(const char* a_title, SectionFunction a_function)
	{
		const int index = g_sectionCount.load();
		if (!a_title || !a_function || index >= static_cast<int>(std::size(g_sections))) {
			return false;
		}
		strncpy_s(g_sections[index].title, a_title, _TRUNCATE);
		g_sections[index].function = a_function;
		g_sectionCount.store(index + 1);
		return true;
	}

	std::uint64_t ProcessReadBytes()
	{
		IO_COUNTERS io{};
		return GetProcessIoCounters(GetCurrentProcess(), &io) ? io.ReadTransferCount : 0;
	}

	bool IsProcessForeground()
	{
		const HWND window = GetForegroundWindow();
		if (!window) {
			return false;
		}
		DWORD processId = 0;
		GetWindowThreadProcessId(window, &processId);
		return processId == GetCurrentProcessId();
	}

	void SetNote(const char* a_note)
	{
		strncpy_s(g_note, a_note ? a_note : "", _TRUNCATE);
	}

	void SetSave(const char* a_text)
	{
		strncpy_s(g_save, a_text ? a_text : "", _TRUNCATE);
	}

	void SetSetup(const char* a_text)
	{
		strncpy_s(g_setup, a_text ? a_text : "", _TRUNCATE);
	}

	bool WriteStallReport(std::uint32_t a_focusThreadId, std::uint32_t a_stallSeconds, StallKind a_kind)
	{
		if (!g_goEvent || g_inCrash || InterlockedCompareExchange(&g_inStall, 1, 0) != 0) {
			return false;
		}

		Out out{ g_stallReport, kReportCap, 0 };
		g_stallReport[0] = '\0';
		session::NoteStall();
		BuildStallReport(out, a_focusThreadId, a_stallSeconds, a_kind);

		SYSTEMTIME now;
		GetLocalTime(&now);
		const wchar_t* kind = a_kind == StallKind::kLoadingScreen ? L"loading" : L"stall";
		wchar_t        path[800];
		swprintf_s(path, L"%s\\%s-%04u-%02u-%02u-%02u-%02u-%02u.log", g_reportDir, kind, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
			now.wSecond);
		// The text report first: the dump can take a long time, and the process can end while
		// the monitor writes it (test of 0.2.0 under Wine: the .dmp was written, the .log not).
		const std::size_t textLen = out.len;
		if (g_writeMinidump) {
			Put(out, "\nMinidump: being written by the monitor process (same file name, .dmp). If this line stays, the game stopped while the dump was written.\n");
		}
		bool ok = WriteReportFile(path, out, nullptr);
		if (g_writeMinidump) {
			// A dump of the frozen game, from the monitor process.
			wchar_t dumpPath[800];
			swprintf_s(dumpPath, L"%s\\%s-%04u-%02u-%02u-%02u-%02u-%02u.dmp", g_reportDir, kind, now.wYear, now.wMonth, now.wDay, now.wHour,
				now.wMinute, now.wSecond);
			const bool dump = monitor::RequestDump(dumpPath, nullptr, a_focusThreadId, 60000);
			out.len = textLen;
			out.buf[out.len] = '\0';
			Put(out, "\nMinidump: %s\n", dump ? "written by the monitor process (same file name, .dmp)" : "not written (the monitor process is not running, or it did not answer in 60 s)");
			ok = WriteReportFile(path, out, nullptr) && ok;
		}
		InterlockedExchange(&g_inStall, 0);
		return ok;
	}

	void SetModLookup(ModLookup a_lookup)
	{
		g_modLookup.store(a_lookup);
	}

	int ListModDlls(ModDll* a_out, int a_max)
	{
		RefreshModules();
		const auto& table = Modules();
		int         count = 0;
		for (int i = 0; i < table.count && count < a_max; ++i) {
			const Module& m = table.items[i];
			if (IsModLibrary(&m)) {
				a_out[count].base = m.base;
				a_out[count].size = m.size;
				strncpy_s(a_out[count].name, m.name, _TRUNCATE);
				++count;
			}
		}
		return count;
	}

	void RefreshModules()
	{
		static HMODULE handles[kMaxModules];
		static wchar_t windowsDir[MAX_PATH];

		AcquireSRWLockExclusive(&g_refreshLock);

		const HANDLE process = GetCurrentProcess();
		DWORD        bytes = 0;
		if (!K32EnumProcessModules(process, handles, sizeof(handles), &bytes)) {
			ReleaseSRWLockExclusive(&g_refreshLock);
			return;
		}
		int count = static_cast<int>(bytes / sizeof(HMODULE));
		if (count > kMaxModules) {
			count = kMaxModules;
		}

		if (windowsDir[0] == L'\0') {
			GetSystemWindowsDirectoryW(windowsDir, MAX_PATH);
		}
		const HMODULE game = GetModuleHandleW(nullptr);
		HMODULE       self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&RefreshModules), &self);

		const int inactive = 1 - g_activeTable.load();
		auto&     table = g_tables[inactive];
		table.count = 0;

		for (int i = 0; i < count; ++i) {
			MODULEINFO info{};
			if (!K32GetModuleInformation(process, handles[i], &info, sizeof(info))) {
				continue;
			}
			wchar_t path[MAX_PATH * 2] = {};
			K32GetModuleFileNameExW(process, handles[i], path, static_cast<DWORD>(std::size(path)));

			auto& m = table.items[table.count++];
			m.base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
			m.size = info.SizeOfImage;
			m.isGame = handles[i] == game;
			m.isSelf = handles[i] == self && self != game;
			const wchar_t* base = wcsrchr(path, L'\\');
			base = base ? base + 1 : path;
			m.isSystem = StartsWithNoCase(path, windowsDir) || IsRuntimeLibrary(base);
			m.isFramework = !m.isSystem && IsFrameworkLibrary(base);
			if (WideCharToMultiByte(CP_UTF8, 0, base, -1, m.name, static_cast<int>(sizeof(m.name)), nullptr, nullptr) == 0) {
				strncpy_s(m.name, "(name not available)", _TRUNCATE);
			}
		}

		g_activeTable.store(inactive, std::memory_order_release);
		ReleaseSRWLockExclusive(&g_refreshLock);
	}

	bool Install(const Config& a_config)
	{
		if (g_goEvent || !a_config.reportDir) {
			return false;
		}

		wcsncpy_s(g_reportDir, a_config.reportDir, _TRUNCATE);
		strncpy_s(g_toolVersion, a_config.toolVersion ? a_config.toolVersion : "", _TRUNCATE);
		strncpy_s(g_gameVersion, a_config.gameVersion ? a_config.gameVersion : "", _TRUNCATE);
		strncpy_s(g_platform, a_config.platform ? a_config.platform : "", _TRUNCATE);
		g_mainThreadId = a_config.mainThreadId;
		g_writeMinidump = a_config.writeMinidump;
		AddVectoredExceptionHandler(1, &CountException);
		g_openReport = a_config.openReport;
		g_startTick = GetTickCount64();

		g_goEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		g_doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!g_goEvent || !g_doneEvent) {
			return false;
		}

		RefreshModules();
		RefreshThreads();

		const HANDLE reporter = CreateThread(nullptr, 256 * 1024, &ReporterThread, nullptr, 0, nullptr);
		if (!reporter) {
			return false;
		}
		CloseHandle(reporter);

		if (const HANDLE maintenance = CreateThread(nullptr, 64 * 1024, &MaintenanceThread, nullptr, 0, nullptr)) {
			CloseHandle(maintenance);
		}

		AssertFilter();
		return true;
	}
}

// ------------------------------------------------------------------ profiler
//
// The profiler is in this file because it uses the lock-free stack walk, the
// module table and the thread table of the crash handler.

namespace skydiag::crash
{
	namespace
	{
		constexpr int   kProfileRing = 16384;  // about 20 s with 8 threads at 100 samples each second
		constexpr int   kProfileFrames = 48;
		constexpr int   kMaxTargets = 8;
		constexpr int   kThreadCache = 256;
		constexpr int   kTaskThreads = 4;
		constexpr float kBusyThread = 0.05f;   // threads below this processor use are not sampled

		struct ProfileSample
		{
			std::int64_t  counter;   // QueryPerformanceCounter value
			std::uint64_t module;    // base address of the module that the thread was in, 0 = not known
			std::uint64_t mod;       // base address of the nearest mod DLL on the call stack, 0 = none
			std::uint32_t threadId;
			std::uint32_t costUs;    // time that the thread was stopped for this sample
			std::uint32_t waiting;   // 1 = the thread was in a system wait function
		};

		ProfileSample     g_profileRing[kProfileRing];
		std::atomic<long> g_profileCount{ 0 };
		HANDLE            g_profileThread = nullptr;
		volatile LONG     g_profileStarted = 0;
		LARGE_INTEGER     g_profileFrequency{};
		const char*       g_profileState = "not started";
		std::atomic<long> g_profileSuspendFailures{ 0 };
		std::atomic<long> g_profileContextFailures{ 0 };

		std::atomic<std::uint32_t> g_renderThreadId{ 0 };
		std::atomic<std::uint32_t> g_taskThreadIds[kTaskThreads];
		std::atomic<long>          g_taskThreadNext{ 0 };
		std::atomic<bool>          g_targetsDirty{ true };

		// Used by the profiler thread only.
		struct CachedThread
		{
			DWORD     id;
			HANDLE    handle;
			ULONGLONG lastCpu;
			float     cpuShare;
			bool      seen;
		};
		CachedThread g_threadCache[kThreadCache];
		int          g_threadCacheCount = 0;

		struct ProfileTarget
		{
			DWORD  id;
			HANDLE handle;
		};
		ProfileTarget g_targets[kMaxTargets];
		int           g_targetCount = 0;

		// The sampled threads and their processor use, for the queries.
		struct PublishedThread
		{
			std::atomic<std::uint32_t> id{ 0 };
			std::atomic<float>         cpuShare{ 0 };
		};
		PublishedThread g_published[kMaxTargets];

		// Address ranges of the system functions in which a thread waits.
		struct WaitRange
		{
			std::uintptr_t begin;
			std::uintptr_t end;
		};
		WaitRange g_waitRanges[24];
		int       g_waitRangeCount = 0;

		void FindWaitFunctions()
		{
			static const char* const ntdllNames[] = { "NtWaitForSingleObject", "NtWaitForMultipleObjects", "NtDelayExecution",
				"NtWaitForAlertByThreadId", "NtWaitForKeyedEvent", "NtSignalAndWaitForSingleObject", "NtRemoveIoCompletion",
				"NtRemoveIoCompletionEx", "NtYieldExecution", "NtWaitForWorkViaWorkerFactory" };
			static const char* const win32uNames[] = { "NtUserMsgWaitForMultipleObjectsEx", "NtUserGetMessage", "NtUserWaitMessage" };

			const auto add = [](HMODULE a_module, const char* a_name) {
				if (!a_module || g_waitRangeCount >= static_cast<int>(std::size(g_waitRanges))) {
					return;
				}
				if (const auto address = reinterpret_cast<std::uintptr_t>(GetProcAddress(a_module, a_name))) {
					g_waitRanges[g_waitRangeCount++] = { address, address + 0x20 };
				}
			};
			const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
			for (const auto name : ntdllNames) {
				add(ntdll, name);
			}
			const HMODULE win32u = GetModuleHandleW(L"win32u.dll");
			for (const auto name : win32uNames) {
				add(win32u, name);
			}
		}

		bool IsWaitAddress(std::uintptr_t a_address)
		{
			for (int i = 0; i < g_waitRangeCount; ++i) {
				if (a_address >= g_waitRanges[i].begin && a_address < g_waitRanges[i].end) {
					return true;
				}
			}
			return false;
		}

		// Fills the module, the nearest mod DLL and the waiting state.
		void ClassifySample(ProfileSample& a_sample, const std::uintptr_t* a_frames, int a_count)
		{
			a_sample.module = 0;
			a_sample.mod = 0;
			a_sample.waiting = 0;
			if (a_count == 0) {
				return;
			}
			if (const Module* top = FindModule(a_frames[0])) {
				a_sample.module = top->base;
			}
			a_sample.waiting = IsWaitAddress(a_frames[0]) ? 1 : 0;
			for (int i = 0; i < a_count; ++i) {
				const Module* m = FindModule(a_frames[i]);
				if (IsModLibrary(m)) {
					a_sample.mod = m->base;
					break;
				}
			}
		}

		bool IsTaskThread(std::uint32_t a_id)
		{
			for (const auto& id : g_taskThreadIds) {
				if (id.load(std::memory_order_relaxed) == a_id) {
					return a_id != 0;
				}
			}
			return false;
		}

		// Measures the processor use of each thread and chooses the threads
		// to sample. Runs one time each second on the profiler thread.
		void UpdateTargets(double a_wallSeconds)
		{
			RefreshThreads();
			const auto& table = g_threadTables[g_activeThreadTable.load(std::memory_order_acquire)];

			for (int i = 0; i < g_threadCacheCount; ++i) {
				g_threadCache[i].seen = false;
			}
			for (int t = 0; t < table.count; ++t) {
				const DWORD   id = table.ids[t];
				CachedThread* entry = nullptr;
				for (int i = 0; i < g_threadCacheCount; ++i) {
					if (g_threadCache[i].id == id) {
						entry = &g_threadCache[i];
						break;
					}
				}
				if (!entry) {
					if (g_threadCacheCount >= kThreadCache) {
						continue;
					}
					const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
					if (!handle) {
						continue;
					}
					entry = &g_threadCache[g_threadCacheCount++];
					entry->id = id;
					entry->handle = handle;
					entry->lastCpu = ThreadCpu100ns(handle);
					entry->cpuShare = 0.0f;
				} else if (a_wallSeconds > 0.0) {
					const ULONGLONG cpu = ThreadCpu100ns(entry->handle);
					entry->cpuShare = static_cast<float>(static_cast<double>(cpu - entry->lastCpu) / 10000000.0 / a_wallSeconds);
					entry->lastCpu = cpu;
				}
				entry->seen = true;
			}
			// Threads that ended.
			for (int i = 0; i < g_threadCacheCount;) {
				if (!g_threadCache[i].seen) {
					CloseHandle(g_threadCache[i].handle);
					g_threadCache[i] = g_threadCache[--g_threadCacheCount];
				} else {
					++i;
				}
			}

			// The render thread and the task threads first, then the busiest.
			const DWORD self = GetCurrentThreadId();
			const DWORD render = g_renderThreadId.load();
			g_targetCount = 0;
			const auto add = [](const CachedThread& a_entry) {
				for (int i = 0; i < g_targetCount; ++i) {
					if (g_targets[i].id == a_entry.id) {
						return;
					}
				}
				if (g_targetCount < kMaxTargets) {
					g_targets[g_targetCount++] = { a_entry.id, a_entry.handle };
				}
			};
			for (int i = 0; i < g_threadCacheCount; ++i) {
				if (g_threadCache[i].id == render) {
					add(g_threadCache[i]);
				}
			}
			for (int i = 0; i < g_threadCacheCount; ++i) {
				if (IsTaskThread(g_threadCache[i].id)) {
					add(g_threadCache[i]);
				}
			}
			static bool used[kThreadCache];
			for (int i = 0; i < g_threadCacheCount; ++i) {
				used[i] = false;
			}
			while (g_targetCount < kMaxTargets) {
				int best = -1;
				for (int i = 0; i < g_threadCacheCount; ++i) {
					if (!used[i] && g_threadCache[i].id != self && g_threadCache[i].cpuShare >= kBusyThread &&
						(best < 0 || g_threadCache[i].cpuShare > g_threadCache[best].cpuShare)) {
						best = i;
					}
				}
				if (best < 0) {
					break;
				}
				used[best] = true;
				add(g_threadCache[best]);
			}

			for (int i = 0; i < kMaxTargets; ++i) {
				if (i < g_targetCount) {
					float share = 0.0f;
					for (int c = 0; c < g_threadCacheCount; ++c) {
						if (g_threadCache[c].id == g_targets[i].id) {
							share = g_threadCache[c].cpuShare;
						}
					}
					g_published[i].cpuShare.store(share);
					g_published[i].id.store(g_targets[i].id);
				} else {
					g_published[i].id.store(0);
				}
			}
		}

		DWORD WINAPI ProfilerThread(LPVOID)
		{
			// A timer with 1 ms precision, if the system has it.
			HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* high resolution */, TIMER_ALL_ACCESS);
			if (!timer) {
				timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
			}

			static std::uintptr_t frames[kProfileFrames];
			LARGE_INTEGER         lastUpdate{};
			for (;;) {
				LARGE_INTEGER due;
				due.QuadPart = -100000;  // 10 ms, in units of 100 ns
				if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
					WaitForSingleObject(timer, 100);
				} else {
					Sleep(10);
				}
				if (g_inCrash) {
					return 0;
				}
				if (g_inStall) {
					continue;  // the stall report stops the threads itself
				}

				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				const double sinceUpdate = static_cast<double>(now.QuadPart - lastUpdate.QuadPart) / static_cast<double>(g_profileFrequency.QuadPart);
				if (lastUpdate.QuadPart == 0 || sinceUpdate >= 1.0 || g_targetsDirty.exchange(false)) {
					UpdateTargets(lastUpdate.QuadPart == 0 ? 0.0 : sinceUpdate);
					lastUpdate = now;
				}

				for (int t = 0; t < g_targetCount; ++t) {
					const HANDLE target = g_targets[t].handle;

					// While the thread is stopped, this code only reads
					// memory and makes system calls.
					LARGE_INTEGER before;
					QueryPerformanceCounter(&before);
					if (SuspendThread(target) == static_cast<DWORD>(-1)) {
						++g_profileSuspendFailures;
						continue;
					}
					CONTEXT context{};
					context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
					int  count = 0;
					bool gotContext = false;
					if (GetThreadContext(target, &context)) {
						gotContext = true;
						count = WalkStack(context, frames, kProfileFrames);
					}
					ResumeThread(target);
					if (!gotContext) {
						++g_profileContextFailures;
						continue;
					}
					LARGE_INTEGER after;
					QueryPerformanceCounter(&after);

					const long     index = g_profileCount.load(std::memory_order_relaxed);
					ProfileSample& sample = g_profileRing[index % kProfileRing];
					sample.counter = before.QuadPart;
					ClassifySample(sample, frames, count);
					sample.costUs = static_cast<std::uint32_t>((after.QuadPart - before.QuadPart) * 1000000 / g_profileFrequency.QuadPart);
					sample.threadId = g_targets[t].id;
					g_profileCount.store(index + 1, std::memory_order_release);
				}
			}
		}

		void ProfileName(char* a_out, std::size_t a_cap, std::uint64_t a_base, bool* a_isMod)
		{
			const Module* m = a_base != 0 ? FindModule(static_cast<std::uintptr_t>(a_base)) : nullptr;
			strncpy_s(a_out, a_cap, m ? m->name : "(code outside all modules)", _TRUNCATE);
			if (a_isMod) {
				*a_isMod = IsModLibrary(m);
			}
		}

		struct ProfileBucket
		{
			std::uint64_t key;
			std::uint32_t hits;     // modules: the thread ran in the module; mods: samples with the mod on the stack
			std::uint32_t own;      // mods only: the thread ran in the mod's own code
			std::uint32_t waiting;  // mods only: the thread waited
		};

		ProfileBucket* FindBucket(ProfileBucket* a_buckets, int& a_count, int a_max, std::uint64_t a_key)
		{
			for (int b = 0; b < a_count; ++b) {
				if (a_buckets[b].key == a_key) {
					return &a_buckets[b];
				}
			}
			if (a_count >= a_max) {
				return nullptr;
			}
			a_buckets[a_count] = { a_key, 0, 0, 0 };
			return &a_buckets[a_count++];
		}

		// Calls a_visit for each sample in the time range, newest first.
		template <class F>
		void ForEachSample(std::int64_t a_from, std::int64_t a_to, F a_visit)
		{
			const long count = g_profileCount.load(std::memory_order_acquire);
			const long stored = count < kProfileRing ? count : kProfileRing;
			for (long i = count - 1; i >= count - stored; --i) {
				const ProfileSample sample = g_profileRing[i % kProfileRing];
				if (sample.counter < a_from) {
					break;
				}
				if (sample.counter <= a_to) {
					a_visit(sample);
				}
			}
		}

		void ProfileRange(profiler::Snapshot& a_out, std::int64_t a_from, std::int64_t a_to, std::uint32_t a_threadId)
		{
			a_out = {};
			if (a_threadId == 0) {
				a_threadId = g_renderThreadId.load();
			}
			if (g_profileFrequency.QuadPart == 0 || a_to <= a_from || a_threadId == 0) {
				return;
			}

			ProfileBucket modules[48];
			ProfileBucket mods[32];
			int           moduleCount = 0;
			int           modCount = 0;
			std::uint32_t waiting = 0;
			std::uint32_t total = 0;
			std::uint64_t costUs = 0;

			ForEachSample(a_from, a_to, [&](const ProfileSample& a_sample) {
				if (a_sample.threadId != a_threadId) {
					return;
				}
				++total;
				costUs += a_sample.costUs;
				if (a_sample.waiting) {
					++waiting;
				} else if (ProfileBucket* bucket = FindBucket(modules, moduleCount, static_cast<int>(std::size(modules)), a_sample.module)) {
					++bucket->hits;
				}
				if (a_sample.mod != 0) {
					if (ProfileBucket* bucket = FindBucket(mods, modCount, static_cast<int>(std::size(mods)), a_sample.mod)) {
						++bucket->hits;
						if (a_sample.waiting) {
							++bucket->waiting;
						} else if (a_sample.module == a_sample.mod) {
							++bucket->own;
						}
					}
				}
			});
			if (total == 0) {
				return;
			}

			const float all = static_cast<float>(total);
			a_out.samples = total;
			a_out.waitShare = static_cast<float>(waiting) / all;
			const double rangeUs = static_cast<double>(a_to - a_from) * 1000000.0 / static_cast<double>(g_profileFrequency.QuadPart);
			a_out.costShare = rangeUs > 0.0 ? static_cast<float>(static_cast<double>(costUs) / rangeUs) : 0.0f;

			// Largest first (selection sort: the lists are short).
			for (int n = 0; n < static_cast<int>(std::size(a_out.entries)) && moduleCount > 0; ++n) {
				int best = 0;
				for (int b = 1; b < moduleCount; ++b) {
					if (modules[b].hits > modules[best].hits) {
						best = b;
					}
				}
				auto& entry = a_out.entries[a_out.count++];
				entry.share = static_cast<float>(modules[best].hits) / all;
				ProfileName(entry.name, sizeof(entry.name), modules[best].key, &entry.isMod);
				modules[best] = modules[--moduleCount];
			}
			for (int n = 0; n < static_cast<int>(std::size(a_out.mods)) && modCount > 0; ++n) {
				int best = 0;
				for (int b = 1; b < modCount; ++b) {
					if (mods[b].hits > mods[best].hits) {
						best = b;
					}
				}
				auto& entry = a_out.mods[a_out.modCount++];
				entry.onStack = static_cast<float>(mods[best].hits) / all;
				entry.ownCode = static_cast<float>(mods[best].own) / all;
				entry.waiting = static_cast<float>(mods[best].waiting) / all;
				ProfileName(entry.name, sizeof(entry.name), mods[best].key, nullptr);
				mods[best] = mods[--modCount];
			}
		}

		int ProfileThreads(profiler::ThreadInfo* a_out, int a_max, std::int64_t a_from, std::int64_t a_to)
		{
			int count = 0;
			struct Counts
			{
				std::uint32_t waiting;
			};
			static thread_local Counts counts[kMaxTargets * 4];

			ForEachSample(a_from, a_to, [&](const ProfileSample& a_sample) {
				int found = -1;
				for (int i = 0; i < count; ++i) {
					if (a_out[i].id == a_sample.threadId) {
						found = i;
						break;
					}
				}
				if (found < 0) {
					if (count >= a_max || count >= static_cast<int>(std::size(counts))) {
						return;
					}
					found = count++;
					a_out[found] = {};
					a_out[found].id = a_sample.threadId;
					counts[found].waiting = 0;
				}
				++a_out[found].samples;
				if (a_sample.waiting) {
					++counts[found].waiting;
				}
			});

			const std::uint32_t render = g_renderThreadId.load();
			for (int i = 0; i < count; ++i) {
				a_out[i].waitShare = static_cast<float>(counts[i].waiting) / static_cast<float>(a_out[i].samples);
				a_out[i].isRender = a_out[i].id == render;
				a_out[i].isTask = IsTaskThread(a_out[i].id);
				for (const auto& published : g_published) {
					if (published.id.load() == a_out[i].id) {
						a_out[i].cpuShare = published.cpuShare.load();
					}
				}
			}
			// Least waiting first.
			for (int i = 0; i < count; ++i) {
				for (int k = i + 1; k < count; ++k) {
					if (a_out[k].waitShare < a_out[i].waitShare) {
						const profiler::ThreadInfo swap = a_out[i];
						a_out[i] = a_out[k];
						a_out[k] = swap;
					}
				}
			}
			return count;
		}

		int ProfileMods(profiler::ModTotal* a_out, int a_max, std::int64_t a_from, std::int64_t a_to)
		{
			ProfileBucket mods[32];
			int           modCount = 0;

			// The number of sample rounds in the range is the largest sample
			// count of one thread.
			struct PerThread
			{
				std::uint32_t id;
				std::uint32_t samples;
			};
			PerThread threads[kMaxTargets * 4];
			int       threadCount = 0;

			ForEachSample(a_from, a_to, [&](const ProfileSample& a_sample) {
				int found = -1;
				for (int i = 0; i < threadCount; ++i) {
					if (threads[i].id == a_sample.threadId) {
						found = i;
						break;
					}
				}
				if (found < 0 && threadCount < static_cast<int>(std::size(threads))) {
					found = threadCount++;
					threads[found] = { a_sample.threadId, 0 };
				}
				if (found >= 0) {
					++threads[found].samples;
				}
				if (a_sample.mod != 0) {
					if (ProfileBucket* bucket = FindBucket(mods, modCount, static_cast<int>(std::size(mods)), a_sample.mod)) {
						++bucket->hits;
						if (a_sample.waiting) {
							++bucket->waiting;
						} else if (a_sample.module == a_sample.mod) {
							++bucket->own;
						}
					}
				}
			});
			std::uint32_t rounds = 0;
			for (int i = 0; i < threadCount; ++i) {
				if (threads[i].samples > rounds) {
					rounds = threads[i].samples;
				}
			}
			if (rounds == 0) {
				return 0;
			}

			int count = 0;
			while (count < a_max && modCount > 0) {
				int best = 0;
				for (int b = 1; b < modCount; ++b) {
					if (mods[b].hits > mods[best].hits) {
						best = b;
					}
				}
				auto& entry = a_out[count++];
				entry.totalCores = static_cast<float>(mods[best].hits) / static_cast<float>(rounds);
				entry.ownCores = static_cast<float>(mods[best].own) / static_cast<float>(rounds);
				entry.waitingCores = static_cast<float>(mods[best].waiting) / static_cast<float>(rounds);
				ProfileName(entry.name, sizeof(entry.name), mods[best].key, nullptr);
				mods[best] = mods[--modCount];
			}
			return count;
		}

		std::int64_t ProfileWindowStart(std::uint64_t a_windowUs, std::int64_t& a_now)
		{
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			a_now = now.QuadPart;
			return now.QuadPart - static_cast<std::int64_t>(a_windowUs) * g_profileFrequency.QuadPart / 1000000;
		}
	}
}

namespace skydiag::profiler
{
	bool Start(std::uint32_t a_renderThreadId)
	{
		using namespace skydiag::crash;
		if (a_renderThreadId == 0 || InterlockedCompareExchange(&g_profileStarted, 1, 0) != 0) {
			return false;
		}
		QueryPerformanceFrequency(&g_profileFrequency);
		FindWaitFunctions();
		g_renderThreadId.store(a_renderThreadId);

		g_profileThread = CreateThread(nullptr, 256 * 1024, &ProfilerThread, nullptr, 0, nullptr);
		if (!g_profileThread) {
			g_profileState = "could not start the helper thread";
			return false;
		}
		g_profileState = "running";
		SetThreadPriority(g_profileThread, THREAD_PRIORITY_ABOVE_NORMAL);
		return true;
	}

	void SetRenderThread(std::uint32_t a_threadId)
	{
		using namespace skydiag::crash;
		if (a_threadId != 0 && g_renderThreadId.load(std::memory_order_relaxed) != a_threadId) {
			g_renderThreadId.store(a_threadId);
			g_targetsDirty.store(true);
		}
	}

	void NoteTaskThread(std::uint32_t a_threadId)
	{
		using namespace skydiag::crash;
		if (a_threadId == 0 || IsTaskThread(a_threadId)) {
			return;
		}
		const long slot = g_taskThreadNext.fetch_add(1) % kTaskThreads;
		g_taskThreadIds[slot].store(a_threadId);
		g_targetsDirty.store(true);
	}

	int ListThreads(ThreadInfo* a_out, int a_max, std::uint64_t a_windowUs)
	{
		using namespace skydiag::crash;
		if (g_profileFrequency.QuadPart == 0) {
			return 0;
		}
		std::int64_t       now = 0;
		const std::int64_t from = ProfileWindowStart(a_windowUs, now);
		return ProfileThreads(a_out, a_max, from, now);
	}

	void GetRange(Snapshot& a_out, std::int64_t a_fromCounter, std::int64_t a_toCounter, std::uint32_t a_threadId)
	{
		skydiag::crash::ProfileRange(a_out, a_fromCounter, a_toCounter, a_threadId);
	}

	void GetSnapshot(Snapshot& a_out, std::uint64_t a_windowUs, std::uint32_t a_threadId)
	{
		using namespace skydiag::crash;
		a_out = {};
		if (g_profileFrequency.QuadPart == 0) {
			return;
		}
		std::int64_t       now = 0;
		const std::int64_t from = ProfileWindowStart(a_windowUs, now);
		ProfileRange(a_out, from, now, a_threadId);
	}

	int GetMods(ModTotal* a_out, int a_max, std::uint64_t a_windowUs)
	{
		using namespace skydiag::crash;
		if (g_profileFrequency.QuadPart == 0) {
			return 0;
		}
		std::int64_t       now = 0;
		const std::int64_t from = ProfileWindowStart(a_windowUs, now);
		return ProfileMods(a_out, a_max, from, now);
	}

	int GetModsRange(ModTotal* a_out, int a_max, std::int64_t a_fromCounter, std::int64_t a_toCounter)
	{
		using namespace skydiag::crash;
		if (g_profileFrequency.QuadPart == 0) {
			return 0;
		}
		return ProfileMods(a_out, a_max, a_fromCounter, a_toCounter);
	}

	std::int64_t Now()
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return now.QuadPart;
	}

	std::int64_t TicksPerSecond()
	{
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		return frequency.QuadPart;
	}

	bool Describe(const Snapshot& a_snapshot, char* a_out, std::size_t a_cap)
	{
		a_out[0] = '\0';
		if (a_snapshot.samples == 0) {
			return false;
		}
		Out  out{ a_out, a_cap, 0 };
		bool first = true;
		for (int i = 0; i < a_snapshot.count && i < 2; ++i) {
			if (a_snapshot.entries[i].share < 0.05f) {
				break;
			}
			Put(out, "%s%.0f%% %s", first ? "" : ", ", a_snapshot.entries[i].share * 100.0f, a_snapshot.entries[i].name);
			first = false;
		}
		if (a_snapshot.waitShare >= 0.05f) {
			Put(out, "%s%.0f%% waiting", first ? "" : ", ", a_snapshot.waitShare * 100.0f);
			first = false;
		}
		if (a_snapshot.modCount > 0 && a_snapshot.mods[0].onStack >= 0.10f) {
			Put(out, "%smod on stack: %s %.0f%%", first ? "" : "; ", a_snapshot.mods[0].name, a_snapshot.mods[0].onStack * 100.0f);
		}
		return true;
	}

	bool DescribeRange(char* a_out, std::size_t a_cap, std::int64_t a_fromCounter, std::int64_t a_toCounter)
	{
		using namespace skydiag::crash;
		a_out[0] = '\0';
		ThreadInfo threads[kMaxTargets * 2];
		const int  count = ProfileThreads(threads, static_cast<int>(std::size(threads)), a_fromCounter, a_toCounter);
		if (count == 0) {
			return false;
		}
		// The list has the thread that waited least first.
		static thread_local Snapshot snapshot;
		ProfileRange(snapshot, a_fromCounter, a_toCounter, threads[0].id);
		return Describe(snapshot, a_out, a_cap);
	}

	void Status(char* a_out, std::size_t a_cap)
	{
		using namespace skydiag::crash;
		Out out{ a_out, a_cap, 0 };
		a_out[0] = '\0';
		int sampled = 0;
		for (const auto& published : g_published) {
			sampled += published.id.load() != 0 ? 1 : 0;
		}
		Put(out, "%s, render thread %lu, %d threads sampled, %ld samples, failures: %ld suspend, %ld context", g_profileState,
			static_cast<unsigned long>(g_renderThreadId.load()), sampled, g_profileCount.load(), g_profileSuspendFailures.load(),
			g_profileContextFailures.load());
	}

	void Append(Out& a_out)
	{
		using namespace skydiag::crash;
		Put(a_out, "\n== GAME THREAD TIME (last 5 s) ==\n");
		if (g_profileFrequency.QuadPart == 0) {
			Put(a_out, "The profiler did not start.\n");
			return;
		}
		std::int64_t       now = 0;
		const std::int64_t from = ProfileWindowStart(5000000, now);

		static ThreadInfo threads[kMaxTargets * 2];
		const int         threadCount = ProfileThreads(threads, static_cast<int>(std::size(threads)), from, now);
		if (threadCount == 0) {
			char status[240];
			Status(status, sizeof(status));
			Put(a_out, "No samples in the last 5 s. Profiler state: %s\n", status);
			return;
		}
		Put(a_out, "The profiler samples the thread that shows the frames, the threads that run SKSE tasks, and the busiest other threads.\n");
		Put(a_out, "'Waiting' means sleep, lock, frame cap, VSync, or a wait for a different thread.\n");

		static Snapshot snapshot;
		for (int t = 0; t < threadCount; ++t) {
			ProfileRange(snapshot, from, now, threads[t].id);
			Put(a_out, "\nThread %lu%s%s: waiting %.1f%%, processor use %.0f%%, %lu samples\n", static_cast<unsigned long>(threads[t].id),
				threads[t].isRender ? " [shows the frames]" : "", threads[t].isTask ? " [runs SKSE tasks]" : "", threads[t].waitShare * 100.0f,
				threads[t].cpuShare * 100.0f, static_cast<unsigned long>(threads[t].samples));
			for (int i = 0; i < snapshot.count; ++i) {
				Put(a_out, "  %5.1f%%  %s%s\n", snapshot.entries[i].share * 100.0f, snapshot.entries[i].name, snapshot.entries[i].isMod ? "  [mod]" : "");
			}
		}

		static ModTotal mods[8];
		const int       modCount = ProfileMods(mods, static_cast<int>(std::size(mods)), from, now);
		if (modCount > 0) {
			Put(a_out, "\nMod DLLs on the call stacks, added up over the sampled threads. The unit is one processor core (100%% = one core busy all the time).\n");
			Put(a_out, "'Total' includes the game and system code that the mod calls or wraps with a hook,\n");
			Put(a_out, "so a mod that hooks a large game function has a large total and small own code.\n");
			Put(a_out, "Own code  Waiting   Total  Mod\n");
			for (int i = 0; i < modCount; ++i) {
				Put(a_out, "%7.1f%%  %6.1f%%  %5.1f%%  %s\n", mods[i].ownCores * 100.0f, mods[i].waitingCores * 100.0f, mods[i].totalCores * 100.0f,
					mods[i].name);
			}
		}
	}
}
