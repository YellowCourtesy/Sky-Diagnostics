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

	namespace
	{
		constexpr char kVersionMark[] = "# Config version:";
		constexpr char kVersionToken[] = "@VERSION@";

		std::string Trim(const std::string& a_text)
		{
			const auto first = a_text.find_first_not_of(" \t\r\n");
			if (first == std::string::npos) {
				return {};
			}
			return a_text.substr(first, a_text.find_last_not_of(" \t\r\n") - first + 1);
		}

		std::string Lower(std::string a_text)
		{
			std::transform(a_text.begin(), a_text.end(), a_text.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
			return a_text;
		}

		// One line of a configuration text, with its place.
		struct Line
		{
			std::string text;
			int         part{ 0 };      // 0 settings, 1 layout, 2 suggestions; -1 = a [[Part]] line
			std::string section;        // lower-case name of the [section] the line is in, empty = none
			bool        header{ false };  // the line is a [section] line
			std::string name;           // settings: lower-case key. layout: lower-case part name. Else empty.
		};

		std::vector<Line> ToLines(const std::string& a_text)
		{
			std::vector<Line>  lines;
			std::istringstream stream(a_text);
			std::string        text;
			int                part = 0;
			std::string        section;
			while (std::getline(stream, text)) {
				if (!text.empty() && text.back() == '\r') {
					text.pop_back();
				}
				Line              line{ text, part, section, false, {} };
				const std::string trimmed = Trim(text);
				std::string       compact;
				for (const char c : trimmed) {
					if (!std::isspace(static_cast<unsigned char>(c))) {
						compact += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
					}
				}
				if (compact == "[[settings]]" || compact == "[[layout]]" || compact == "[[suggestions]]") {
					part = compact == "[[settings]]" ? 0 : (compact == "[[layout]]" ? 1 : 2);
					section.clear();
					line.part = -1;
					line.section.clear();
				} else if (!trimmed.empty() && trimmed.front() == '[') {
					const auto close = trimmed.find(']');
					section = Lower(Trim(trimmed.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
					line.section = section;
					line.header = true;
				} else if (!trimmed.empty() && trimmed.front() != '#' && part != 2) {
					std::string body = trimmed.substr(0, trimmed.find('#'));
					if (part == 0) {
						const auto equals = body.find('=');
						if (equals != std::string::npos) {
							line.name = Lower(Trim(body.substr(0, equals)));
						}
					} else {
						line.name = Lower(Trim(body.substr(0, body.find_first_of(" \t"))));
					}
				}
				lines.push_back(std::move(line));
			}
			return lines;
		}

		bool IsComment(const std::string& a_text)
		{
			const std::string trimmed = Trim(a_text);
			return !trimmed.empty() && trimmed.front() == '#';
		}

		// The place after the last line with text in [section] of a part,
		// or -1 if the part has no such section.
		int EndOfSection(const std::vector<Line>& a_lines, int a_part, const std::string& a_section)
		{
			int end = -1;
			for (int i = 0; i < static_cast<int>(a_lines.size()); ++i) {
				const Line& line = a_lines[i];
				if (line.part == a_part && line.section == a_section && !Trim(line.text).empty()) {
					end = i + 1;
				}
			}
			return end;
		}

		// The place after the last line with text of a part, or -1 if the
		// text has no lines of the part.
		int EndOfPart(const std::vector<Line>& a_lines, int a_part)
		{
			int  end = -1;
			bool inPart = a_part == 0;  // text before the first [[Part]] line is settings text
			for (int i = 0; i < static_cast<int>(a_lines.size()); ++i) {
				const Line& line = a_lines[i];
				if (line.part == -1) {
					inPart = Lower(Trim(line.text)).find(a_part == 0 ? "settings" : (a_part == 1 ? "layout" : "suggestions")) != std::string::npos;
					if (inPart) {
						end = i + 1;
					}
					continue;
				}
				if (inPart && line.part == a_part && !Trim(line.text).empty()) {
					end = i + 1;
				}
			}
			return end;
		}

		void Insert(std::vector<Line>& a_lines, int a_at, const std::vector<std::string>& a_texts, int a_part, const std::string& a_section)
		{
			std::vector<Line> add;
			for (const auto& text : a_texts) {
				add.push_back(Line{ text, a_part, a_section, false, {} });
			}
			a_lines.insert(a_lines.begin() + a_at, add.begin(), add.end());
		}

		// The comment lines just above line a_index (no blank line between).
		std::vector<std::string> CommentsAbove(const std::vector<Line>& a_lines, int a_index)
		{
			int first = a_index;
			while (first > 0 && IsComment(a_lines[first - 1].text) && !a_lines[first - 1].header) {
				--first;
			}
			std::vector<std::string> texts;
			for (int i = first; i < a_index; ++i) {
				texts.push_back(a_lines[i].text);
			}
			return texts;
		}

		// The section header line text and its comments, from the default text.
		std::vector<std::string> SectionStart(const std::vector<Line>& a_lines, int a_part, const std::string& a_section)
		{
			for (int i = 0; i < static_cast<int>(a_lines.size()); ++i) {
				if (a_lines[i].part == a_part && a_lines[i].header && a_lines[i].section == a_section) {
					std::vector<std::string> texts = CommentsAbove(a_lines, i);
					texts.push_back(a_lines[i].text);
					return texts;
				}
			}
			return { "[" + a_section + "]" };
		}
	}

	std::string DefaultTextFor(const std::string& a_version)
	{
		std::string text = DefaultText();
		if (const auto at = text.find(kVersionToken); at != std::string::npos) {
			text.replace(at, sizeof(kVersionToken) - 1, a_version);
		}
		return text;
	}

	std::string VersionOf(const std::string& a_text)
	{
		for (const auto& line : ToLines(a_text)) {
			const std::string trimmed = Trim(line.text);
			if (trimmed.rfind(kVersionMark, 0) == 0) {
				return Trim(trimmed.substr(sizeof(kVersionMark) - 1));
			}
		}
		return {};
	}

	UpdateResult Update(const std::string& a_text, const std::string& a_version)
	{
		UpdateResult result;
		result.text = a_text;
		result.oldVersion = VersionOf(a_text);
		if (result.oldVersion == a_version) {
			return result;
		}
		std::vector<Line>       lines = ToLines(a_text);
		const std::vector<Line> defaults = ToLines(DefaultTextFor(a_version));

		// What the text has now.
		const auto has = [&](int a_part, const std::string& a_section, const std::string& a_name) {
			for (const auto& line : lines) {
				if (line.part != a_part) {
					continue;
				}
				// Settings keys are unique in the file; layout parts too. The
				// section of a key or a part does not matter for "has".
				if (!a_name.empty() && line.name == a_name) {
					return true;
				}
				if (a_name.empty() && line.header && line.section == a_section) {
					return true;
				}
			}
			return false;
		};

		// Whole parts that the text does not have: add them at the end.
		const char* partNames[3] = { "[[Settings]]", "[[Layout]]", "[[Suggestions]]" };
		for (int part = 1; part <= 2; ++part) {
			if (EndOfPart(lines, part) >= 0) {
				continue;
			}
			std::vector<std::string> texts{ "", partNames[part] };
			bool                     in = false;
			for (const auto& line : defaults) {
				if (line.part == -1) {
					in = Lower(Trim(line.text)) == Lower(partNames[part]);
					continue;
				}
				if (in) {
					texts.push_back(line.text);
				}
			}
			Insert(lines, static_cast<int>(lines.size()), texts, part, "");
			lines = ToLines([&] {
				std::string all;
				for (const auto& l : lines) {
					all += l.text + "\n";
				}
				return all;
			}());
			result.added.push_back(std::string("part ") + partNames[part]);
		}

		for (int i = 0; i < static_cast<int>(defaults.size()); ++i) {
			const Line& line = defaults[i];
			if (line.part == 0 || line.part == 1) {
				if (line.name.empty() || has(line.part, line.section, line.name)) {
					continue;
				}
				// A setting or a layout part: with its comment lines, at the
				// end of its section; a section that is missing comes too.
				std::vector<std::string> texts = line.part == 0 ? CommentsAbove(defaults, i) : std::vector<std::string>{};
				texts.push_back(line.text);
				int at = EndOfSection(lines, line.part, line.section);
				if (at < 0) {
					std::vector<std::string> start = SectionStart(defaults, line.part, line.section);
					start.insert(start.begin(), "");
					texts.insert(texts.begin(), start.begin(), start.end());
					at = EndOfPart(lines, line.part);
					if (at < 0 && line.part == 0) {
						// No settings text: before the first [[Part]] line.
						at = 0;
						while (at < static_cast<int>(lines.size()) && lines[at].part != -1) {
							++at;
						}
					} else if (at < 0) {
						at = static_cast<int>(lines.size());
					}
				} else if (line.part == 0 && !texts.empty() && IsComment(texts.front())) {
					texts.insert(texts.begin(), "");  // a blank line before the comments
				}
				if (line.part == 0 && at < static_cast<int>(lines.size()) && (lines[at].header || lines[at].part == -1)) {
					texts.push_back("");  // a blank line before the next section
				}
				Insert(lines, at, texts, line.part, line.section);
				result.added.push_back("[" + line.section + "] " + Trim(line.text.substr(0, line.text.find_first_of("=#"))));
			} else if (line.part == 2 && line.header && !has(2, line.section, "")) {
				// A suggestion section: all of it, at the end of the part.
				std::vector<std::string> texts{ "", line.text };
				for (int j = i + 1; j < static_cast<int>(defaults.size()) && defaults[j].part == 2 && !defaults[j].header; ++j) {
					if (!Trim(defaults[j].text).empty()) {
						texts.push_back(defaults[j].text);
					}
				}
				int at = EndOfPart(lines, 2);
				Insert(lines, at < 0 ? static_cast<int>(lines.size()) : at, texts, 2, line.section);
				result.added.push_back("suggestions [" + line.section + "]");
			}
		}

		// The version line: changed in place, or added as the second line.
		std::string versionLine = std::string(kVersionMark) + " " + a_version;
		bool        found = false;
		for (auto& l : lines) {
			if (Trim(l.text).rfind(kVersionMark, 0) == 0) {
				l.text = versionLine;
				found = true;
				break;
			}
		}
		if (!found) {
			// After the title comment, or at the top.
			Insert(lines, !lines.empty() && IsComment(lines[0].text) ? 1 : 0, { versionLine }, 0, "");
		}

		result.text.clear();
		for (const auto& l : lines) {
			result.text += l.text + "\n";
		}
		result.changed = true;
		return result;
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

	bool WriteDefaultIfMissing(const std::filesystem::path& a_path, const std::string& a_version)
	{
		std::error_code ec;
		if (std::filesystem::exists(a_path, ec)) {
			return false;
		}
		std::filesystem::create_directories(a_path.parent_path(), ec);
		{
			std::ofstream out(a_path, std::ios::binary);
			out << DefaultTextFor(a_version);
			out.flush();
			if (!out) {
				return false;
			}
		}
		// Make sure that the file is there: a write to a folder that the
		// process cannot change can fail with no error.
		return std::filesystem::exists(a_path, ec);
	}

	UpdateResult UpdateFile(const std::filesystem::path& a_path, const std::string& a_version, std::string& a_error)
	{
		a_error.clear();
		std::string text;
		{
			std::ifstream file(a_path, std::ios::binary);
			if (!file) {
				a_error = "could not read " + a_path.string();
				return {};
			}
			std::stringstream stream;
			stream << file.rdbuf();
			text = stream.str();
		}
		UpdateResult result = Update(text, a_version);
		if (!result.changed) {
			return result;
		}
		// A new file first, then in the place of the old one: a crash in the
		// write does not damage the file.
		std::filesystem::path temporary = a_path;
		temporary += ".tmp";
		{
			std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
			out << result.text;
			out.flush();
			if (!out) {
				a_error = "could not write " + temporary.string();
				return result;
			}
		}
		std::error_code ec;
		std::filesystem::rename(temporary, a_path, ec);
		if (ec) {
			std::error_code copyEc;
			std::filesystem::copy_file(temporary, a_path, std::filesystem::copy_options::overwrite_existing, copyEc);
			std::filesystem::remove(temporary, ec);
			if (copyEc) {
				a_error = "could not replace " + a_path.string() + " (" + copyEc.message() + ")";
			}
		}
		return result;
	}
}
