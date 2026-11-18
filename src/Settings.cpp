#include "Settings.h"

#include "Config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace skydiag
{
	namespace
	{
		std::string Trim(const std::string& a_text)
		{
			std::size_t begin = 0;
			std::size_t end = a_text.size();
			while (begin < end && std::isspace(static_cast<unsigned char>(a_text[begin]))) {
				++begin;
			}
			while (end > begin && std::isspace(static_cast<unsigned char>(a_text[end - 1]))) {
				--end;
			}
			return a_text.substr(begin, end - begin);
		}

		std::string Lower(std::string a_text)
		{
			std::transform(a_text.begin(), a_text.end(), a_text.begin(), [](unsigned char a_char) { return static_cast<char>(std::tolower(a_char)); });
			return a_text;
		}

		// Reads a number. Accepts decimal and 0x hexadecimal. Returns false
		// if the text is not a number.
		bool Number(const std::string& a_text, double& a_out)
		{
			if (a_text.empty()) {
				return false;
			}
			char* end = nullptr;
			if (a_text.size() > 2 && a_text[0] == '0' && (a_text[1] == 'x' || a_text[1] == 'X')) {
				a_out = static_cast<double>(std::strtol(a_text.c_str(), &end, 16));
			} else {
				a_out = std::strtod(a_text.c_str(), &end);
			}
			return end && *end == '\0';
		}
	}

	Settings ParseSettings(const std::string& a_text)
	{
		Settings           result;
		std::istringstream stream(a_text);
		std::string        line;
		while (std::getline(stream, line)) {
			const auto comment = line.find_first_of(";#");
			if (comment != std::string::npos) {
				line.erase(comment);
			}
			const auto equals = line.find('=');
			if (equals == std::string::npos) {
				continue;  // empty line or section name
			}
			const std::string key = Lower(Trim(line.substr(0, equals)));
			const std::string value = Lower(Trim(line.substr(equals + 1)));
			double            number = 0.0;
			const bool        isNumber = Number(value, number);

			if (key == "view") {
				if (value == "all") {
					result.view = 0;
				} else if (value == "warnings") {
					result.view = 1;
				} else if (value == "off") {
					result.view = 2;
				}
			} else if (key == "togglekey" && isNumber && number >= 1 && number <= 254) {
				result.toggleKey = static_cast<int>(number);
			} else if (key == "warningholdseconds" && isNumber && number >= 0 && number <= 600) {
				result.warningHoldSeconds = static_cast<float>(number);
			} else if (key == "fpsbelow" && isNumber && number >= 1 && number <= 1000) {
				result.fpsBelow = static_cast<float>(number);
			} else if (key == "profiler" && isNumber) {
				result.profiler = number != 0;
			} else if (key == "papyrus" && isNumber) {
				result.papyrus = number != 0;
			} else if (key == "enginelimits" && isNumber) {
				result.engineLimits = number != 0;
			} else if (key == "animations" && isNumber) {
				result.animations = number != 0;
			} else if (key == "papyrustime" && isNumber) {
				result.papyrusTime = number != 0;
			} else if (key == "memorybydll" && isNumber) {
				result.memoryByDll = number != 0;
			} else if (key == "overlaytarget") {
				if (value == "game") {
					result.overlayOnGameTarget = true;
				} else if (value == "swapchain") {
					result.overlayOnGameTarget = false;
				}
			} else if (key == "stallseconds" && isNumber && number >= 5 && number <= 3600) {
				result.stallSeconds = static_cast<int>(number);
			}
		}
		return result;
	}

	Settings LoadSettings(const std::string& a_path)
	{
		bool     found = false;
		Settings result = ParseSettings(config::Load(a_path, &found).settings);
		result.fileFound = found;
		return result;
	}
}
