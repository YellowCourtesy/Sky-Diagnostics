#pragma once

// The configuration file. One file has the settings, the layout of the
// overlay and the suggestions of the session report. The default text is
// the file SkyrimDiagnostics.ini at the top level of the source tree: the
// build puts it into the program. Standard C++ only.

#include <filesystem>
#include <string>

namespace skydiag::config
{
	// The text of the default configuration file.
	const char* DefaultText();

	struct Parts
	{
		std::string settings;
		std::string layout;
		std::string suggestions;
	};

	// Divides a configuration text at the lines [[Settings]], [[Layout]]
	// and [[Suggestions]]. Text before the first of these lines is
	// settings text (the file format of older versions). A part that the
	// text does not have comes from the default text.
	Parts Split(const std::string& a_text);

	// Reads the file and divides it. If the file does not exist or cannot
	// be read, the result is the default configuration.
	Parts Load(const std::filesystem::path& a_path, bool* a_found = nullptr);

	// Makes the file with the default text if it does not exist. Returns
	// true if the file was made.
	bool WriteDefaultIfMissing(const std::filesystem::path& a_path);
}
