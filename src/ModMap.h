#pragma once

// Which mod a file comes from. The data comes from the Mod Organizer 2
// plugin (mo2/skyrim_diagnostics_export.py), which writes owners.tsv when
// the game starts. Standard C++ only.
//
// The lookup functions use no heap memory and no lock, so the crash report
// can call them.

#include <filesystem>

namespace skydiag::modmap
{
	// Reads owners.tsv and mods.tsv from a_dir. Returns false if owners.tsv
	// does not exist or has no lines. Call again to read new files.
	bool Load(const std::filesystem::path& a_dir);

	bool Loaded();
	int  FileCount();
	int  ModCount();

	// The mod that gives the file. a_path is relative to the Data folder,
	// with any letter case and "/" or "\". Returns "" if not known.
	const char* ModForFile(const char* a_path);

	// "Foo.dll" -> the mod of skse\plugins\foo.dll, or of foo.dll in Data.
	const char* ModForDll(const char* a_name);
	// "FooScript" -> the mod of scripts\fooscript.pex. Scripts that are in a
	// BSA archive or in the game folder itself are not known.
	const char* ModForScript(const char* a_name);
}
