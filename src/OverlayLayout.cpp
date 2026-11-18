#include "OverlayLayout.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace skydiag::layout
{
	namespace
	{
		const std::pair<const char*, Part> kNames[] = {
			{ "fps", Part::kFps },
			{ "graph", Part::kGraph },
			{ "present", Part::kPresent },
			{ "limit", Part::kLimit },
			{ "drawcalls", Part::kDrawCalls },
			{ "latency", Part::kLatency },
			{ "response", Part::kResponse },
			{ "memory", Part::kMemory },
			{ "vram", Part::kVram },
			{ "references", Part::kReferences },
			{ "plugins", Part::kPlugins },
			{ "animations", Part::kAnimations },
			{ "modmemory", Part::kModMemory },
			{ "threads", Part::kThreads },
			{ "moddlls", Part::kModDlls },
			{ "papyrus", Part::kPapyrus },
			{ "scripts", Part::kScripts },
			{ "functions", Part::kFunctions },
		};
	}

	std::vector<Part> Parse(const std::string& a_text)
	{
		std::vector<Part>  result;
		std::istringstream stream(a_text);
		std::string        line;
		bool               sectionOn = true;
		bool               separatorDue = false;
		while (std::getline(stream, line)) {
			const auto comment = line.find('#');
			if (comment != std::string::npos) {
				line.erase(comment);
			}
			std::transform(line.begin(), line.end(), line.begin(), [](unsigned char a_char) { return static_cast<char>(std::tolower(a_char)); });
			std::istringstream words(line);
			std::string        first, second;
			if (!(words >> first)) {
				continue;
			}
			if (first.front() == '[') {
				// The section name can have spaces: "off" is the text after "]".
				const auto close = line.find(']');
				if (close == std::string::npos) {
					continue;
				}
				std::istringstream rest(line.substr(close + 1));
				rest >> second;
				sectionOn = second != "off";
				separatorDue = true;
				continue;
			}
			words >> second;
			if (!sectionOn || second == "off") {
				continue;
			}
			for (const auto& [name, part] : kNames) {
				if (first == name) {
					if (separatorDue && !result.empty()) {
						result.push_back(Part::kSeparator);
					}
					separatorDue = false;
					result.push_back(part);
					break;
				}
			}
		}
		return result;
	}
}
