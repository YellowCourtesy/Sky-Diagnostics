#pragma once

// The configuration file. One file has the settings, the layout of the
// overlay and the suggestions of the session report. The default text is
// the file SkyrimDiagnostics.ini at the top level of the source tree: the
// build puts it into the program. Standard C++ only.

#include <filesystem>
#include <string>
#include <vector>

namespace skydiag::config
{
	// The text of the default configuration file. Its version line has the
	// text @VERSION@ in place of the version.
	const char* DefaultText();

	// The default text with a_version in its version line.
	std::string DefaultTextFor(const std::string& a_version);

	// The version in the line "# Config version: x" of a_text, or empty.
	std::string VersionOf(const std::string& a_text);

	struct UpdateResult
	{
		std::string              text;   // the updated text
		std::string              oldVersion;  // the version of the text before, empty = no version line
		std::vector<std::string> added;  // what was added, for the log ("[Overlay] WarningHoldSeconds", ...)
		bool                     changed{ false };
	};

	// Adds to a_text what the default text of a_version has and a_text does
	// not have, in place: settings keys (with their comment lines) at the
	// end of their [Section], layout parts in their [section], suggestion
	// sections, and parts ([[Layout]], ...) that are missing. Values,
	// comments and order of a_text stay. Sets the version line to
	// a_version (adds it if there is none). If the version of a_text is
	// a_version, nothing changes.
	UpdateResult Update(const std::string& a_text, const std::string& a_version);

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
	bool WriteDefaultIfMissing(const std::filesystem::path& a_path, const std::string& a_version);

	// Update() on the file. Writes the file only if it changed. a_error is
	// empty if all is correct.
	UpdateResult UpdateFile(const std::filesystem::path& a_path, const std::string& a_version, std::string& a_error);
}
