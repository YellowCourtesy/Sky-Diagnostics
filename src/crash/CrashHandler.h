#pragma once

// Crash handler core.
//
// This component uses only the Win32 API and the C runtime. It does not use
// CommonLibSSE or game data, so it can run before the game is initialized
// and it can be tested without the game.

#include <cstddef>
#include <cstdint>

namespace skydiag::crash
{
	struct Config
	{
		const wchar_t* reportDir{ nullptr };    // must exist
		const char*    toolVersion{ "" };
		const char*    gameVersion{ "" };
		const char*    platform{ "" };
		std::uint32_t  mainThreadId{ 0 };       // 0 = unknown
		bool           writeMinidump{ true };
	};

	// Installs the handler. Call one time, not from DllMain.
	bool Install(const Config& a_config);

	// A describer gives game data for an object that the report found.
	// a_subobject points to the base class that the describer was
	// registered for. a_text gets a short description. a_source gets the
	// file that the object comes from (for example a plugin file), or stays
	// empty. The function runs after a crash: it must not allocate memory
	// and must not take locks. Faults in it are caught.
	using Describer = void (*)(void* a_subobject, char* a_text, std::size_t a_textCap, char* a_source, std::size_t a_sourceCap);

	// Registers a describer for all classes that have a_baseClass as a base
	// class (or are that class). The name is the C++ name, for example
	// "TESForm" or "std::exception". Call before a crash, from one thread.
	bool RegisterDescriber(const char* a_baseClass, Describer a_describer);

	// Gets the C++ class name of an object with a virtual function table.
	// Returns false if the pointer is not such an object.
	bool ClassName(const void* a_object, char* a_out, std::size_t a_cap);

	// A section function writes extra text for crash and stall reports
	// (at most a_cap characters, zero-terminated). It runs after a crash:
	// no heap memory, no blocking locks. Faults in it are caught.
	using SectionFunction = void (*)(char* a_out, std::size_t a_cap);
	bool RegisterSection(const char* a_title, SectionFunction a_function);

	// Gives the name of the mod that a DLL file comes from, or "" if not
	// known (data of the mod manager). The function runs after a crash: no
	// heap memory, no locks. The reports show the mod next to the DLL.
	using ModLookup = const char* (*)(const char* a_dllName);
	void SetModLookup(ModLookup a_lookup);

	// Adds one line of text to the top of the next report (for example
	// "test crash"). The text is copied.
	void SetNote(const char* a_note);

	// Makes a report with the call stack of each thread. Use it when a_focusThreadId
	// (the game update thread) did not respond for a_stallSeconds. The
	// threads continue after the report. Returns true if a file was written.
	bool WriteStallReport(std::uint32_t a_focusThreadId, std::uint32_t a_stallSeconds);

	// True if the active (foreground) window belongs to this process. The
	// game stops its update loop while its window is not active.
	bool IsProcessForeground();

	// The mod DLLs that are loaded now (not the game, system, framework or
	// this tool). Returns the count. Not from a crash context.
	struct ModDll
	{
		std::uintptr_t base;
		std::uintptr_t size;
		char           name[96];
	};
	int ListModDlls(ModDll* a_out, int a_max);

	// Updates the module list that the report uses. The handler also does
	// this periodically. Do not call from a crash context.
	void RefreshModules();
}
