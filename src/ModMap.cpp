#include "ModMap.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace skydiag::modmap
{
	namespace
	{
		struct Owner
		{
			std::string path;  // lower case, "\" separators
			std::string mod;
		};

		struct Data
		{
			std::vector<Owner> owners;  // sorted by path
			int                modCount{ 0 };
		};

		// A table that was published stays in memory: a reader can still use it.
		std::atomic<const Data*> g_data{ nullptr };

		// Lower case and "\" separators, into a fixed buffer. False if too long.
		bool Normalize(const char* a_in, char* a_out, std::size_t a_cap)
		{
			std::size_t n = 0;
			for (; a_in[n] != '\0'; ++n) {
				if (n + 1 >= a_cap) {
					return false;
				}
				char c = a_in[n];
				if (c >= 'A' && c <= 'Z') {
					c = static_cast<char>(c - 'A' + 'a');
				} else if (c == '/') {
					c = '\\';
				}
				a_out[n] = c;
			}
			a_out[n] = '\0';
			return n > 0;
		}

		const char* Find(const char* a_key)
		{
			const Data* data = g_data.load();
			if (!data) {
				return "";
			}
			std::size_t low = 0, high = data->owners.size();
			while (low < high) {
				const std::size_t middle = (low + high) / 2;
				const int         order = std::strcmp(data->owners[middle].path.c_str(), a_key);
				if (order == 0) {
					return data->owners[middle].mod.c_str();
				}
				if (order < 0) {
					low = middle + 1;
				} else {
					high = middle;
				}
			}
			return "";
		}
	}

	bool Load(const std::filesystem::path& a_dir)
	{
		std::ifstream file(a_dir / "owners.tsv");
		if (!file) {
			return false;
		}
		auto        data = new Data;
		std::string line;
		char        key[520];
		while (std::getline(file, line)) {
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
				line.pop_back();
			}
			const auto tab = line.find('\t');
			if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) {
				continue;
			}
			const auto end = line.find('\t', tab + 1);
			if (!Normalize(line.substr(0, tab).c_str(), key, sizeof(key))) {
				continue;
			}
			data->owners.push_back({ key, line.substr(tab + 1, end == std::string::npos ? std::string::npos : end - tab - 1) });
		}
		if (data->owners.empty()) {
			delete data;
			return false;
		}
		std::sort(data->owners.begin(), data->owners.end(), [](const Owner& a_lhs, const Owner& a_rhs) { return a_lhs.path < a_rhs.path; });

		std::ifstream mods(a_dir / "mods.tsv");
		while (std::getline(mods, line)) {
			data->modCount += line.empty() ? 0 : 1;
		}
		g_data.store(data);
		return true;
	}

	bool Loaded()
	{
		return g_data.load() != nullptr;
	}

	int FileCount()
	{
		const Data* data = g_data.load();
		return data ? static_cast<int>(data->owners.size()) : 0;
	}

	int ModCount()
	{
		const Data* data = g_data.load();
		return data ? data->modCount : 0;
	}

	const char* ModForFile(const char* a_path)
	{
		char key[520];
		if (!a_path || !Normalize(a_path, key, sizeof(key))) {
			return "";
		}
		return Find(key);
	}

	const char* ModForDll(const char* a_name)
	{
		char key[520] = "skse\\plugins\\";
		const std::size_t prefix = std::strlen(key);
		if (!a_name || !Normalize(a_name, key + prefix, sizeof(key) - prefix)) {
			return "";
		}
		const char* mod = Find(key);
		return mod[0] != '\0' ? mod : Find(key + prefix);
	}

	const char* ModForScript(const char* a_name)
	{
		char key[520] = "scripts\\";
		const std::size_t prefix = std::strlen(key);
		if (!a_name || !Normalize(a_name, key + prefix, sizeof(key) - prefix)) {
			return "";
		}
		const std::size_t length = std::strlen(key);
		if (length + 5 >= sizeof(key)) {
			return "";
		}
		std::memcpy(key + length, ".pex", 5);
		return Find(key);
	}
}
