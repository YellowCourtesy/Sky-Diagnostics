#pragma once

// Known crash signs from the data of Phostwood's Crash Log Analyzer
// (https://github.com/Phostwood/crash-analyzer, GPL-3.0, by Phostwood).
//
// Only its data tables are used (tools/phostwood.py): names that are in a
// crash log when a known problem is the cause, DLL versions that do not work
// with newer game versions, short explanations of file names, and the files
// of antivirus and overlay programs. The tests here are written for the
// sections of this tool's crash report.

#include "Text.h"

#include <cstddef>

namespace skydiag::crash::phostwood
{
	struct Span
	{
		const char* text{ nullptr };
		std::size_t len{ 0 };
	};

	struct CrashInput
	{
		Span               exception;       // the EXCEPTION section
		Span               callStackAndRegs; // the CALL STACK and REGISTERS sections
		Span               top;             // CALL STACK, REGISTERS and STACK sections
		const char*        gameOffset{ "" }; // crash address in the game executable (hex, no 0x), or ""
		const char* const* loaded{ nullptr };  // names of all loaded modules
		int                loadedCount{ 0 };
		const char* const* onStack{ nullptr }; // names of the modules on the call stack, nearest to the crash first, no duplicates
		int                onStackCount{ 0 };
	};

	// The source of the data: commit and date of the analyzer.
	const char* Commit();
	const char* Date();

	// Normal code, before a crash: checks the versions of the loaded DLLs
	// that the data has rules for. a_gameVersion: for example "1.6.1170.0"
	// (the first number in the text is used). Writes one line for each DLL
	// that does not work with this game version to a_out, and keeps the text
	// for the crash reports. Returns the number of these DLLs.
	int CheckDllVersions(const char* a_gameVersion, Out& a_out);

	// What Append found (bits of its return value).
	constexpr int kFoundSigns = 1;     // a known crash sign
	constexpr int kFoundPrograms = 2;  // an antivirus or overlay program in the game process
	constexpr int kFoundDllVersion = 4; // a DLL version that does not work with this game version

	// After a crash (no heap memory, no locks): writes the known crash signs.
	int Append(Out& a_out, const CrashInput& a_in);
}
