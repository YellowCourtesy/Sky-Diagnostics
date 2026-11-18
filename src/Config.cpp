#include "Config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "DefaultConfig.inc"

namespace skydiag::config
{
	namespace
	{
		enum class Section
		{
			kSettings,
			kLayout,
			kSuggestions,
		};

		// The three texts of a_text, and which of them a_text has.
		void Divide(const std::string& a_text, std::string a_out[3], bool a_has[3])
		{
			std::istringstream stream(a_text);
			std::string        line;
			Section            section = Section::kSettings;
			while (std::getline(stream, line)) {
				std::string name;
				for (const char c : line) {
					if (!std::isspace(static_cast<unsigned char>(c))) {
						name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
					}
				}
				if (name == "[[settings]]") {
					section = Section::kSettings;
					a_has[0] = true;
				} else if (name == "[[layout]]") {
					section = Section::kLayout;
					a_has[1] = true;
				} else if (name == "[[suggestions]]") {
					section = Section::kSuggestions;
					a_has[2] = true;
				} else {
					a_out[static_cast<int>(section)] += line;
					a_out[static_cast<int>(section)] += '\n';
				}
			}
		}
	}

	const char* DefaultText()
	{
		return reinterpret_cast<const char*>(kDefaultConfig);
	}

	Parts Split(const std::string& a_text)
	{
		std::string text[3];
		bool        has[3] = { true, false, false };  // text with no part line is settings text
		Divide(a_text, text, has);

		std::string defaults[3];
		bool        unused[3] = {};
		Divide(DefaultText(), defaults, unused);

		Parts parts;
		parts.settings = text[0];
		parts.layout = has[1] ? text[1] : defaults[1];
		parts.suggestions = has[2] ? text[2] : defaults[2];
		return parts;
	}

	Parts Load(const std::filesystem::path& a_path, bool* a_found)
	{
		std::ifstream file(a_path);
		if (a_found) {
			*a_found = static_cast<bool>(file);
		}
		if (!file) {
			return Split(DefaultText());
		}
		std::stringstream text;
		text << file.rdbuf();
		return Split(text.str());
	}

	bool WriteDefaultIfMissing(const std::filesystem::path& a_path)
	{
		std::error_code ec;
		if (std::filesystem::exists(a_path, ec)) {
			return false;
		}
		std::filesystem::create_directories(a_path.parent_path(), ec);
		{
			std::ofstream out(a_path, std::ios::binary);
			out << DefaultText();
			out.flush();
			if (!out) {
				return false;
			}
		}
		// Make sure that the file is there: a write to a folder that the
		// process cannot change can fail with no error.
		return std::filesystem::exists(a_path, ec);
	}
}
