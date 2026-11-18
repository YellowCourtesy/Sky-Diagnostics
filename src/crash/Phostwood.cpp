#include "Phostwood.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>

// Rules for this file: Append runs after a crash. It must not allocate heap
// memory and must not take locks. CheckDllVersions is normal code.

namespace skydiag::crash::phostwood
{
	namespace
	{
		struct PhostwoodItem
		{
			const char* text;
			const char* what;
		};
		struct PhostwoodGroup
		{
			const char* key;
			int         first;  // in kPhostwoodItems
			int         count;  // names to find in the report
			int         hex;    // then this many exception address offsets
			const char* requiresModule;  // the group counts only if this DLL is loaded ("" = none)
			const char* requiresText;    // the group counts only if this text is at the crash place ("" = none)
			bool        possibleOnly;    // never "probable"
			const char* conditionAdded;  // the date the condition was written into tools/phostwood.py
		};
		struct PhostwoodDllVersion
		{
			const char*   dll;
			std::uint16_t version[4];      // this version and older ones do not work with newer games
			std::uint16_t maxGame[4];      // the newest game version that they work with (0 = not known)
			std::uint16_t ignoreBelow[4];  // older versions than this are not checked (0 = all)
			const char*   recommended;
			const char*   mod;
			const char*   url;
			const char*   note;
		};
		struct PhostwoodExplanation
		{
			const char* file;
			const char* what;
		};
		struct PhostwoodProductFile
		{
			const char* product;
			const char* file;
		};

#include "PhostwoodData.inc"

		char g_dllText[4096];  // CheckDllVersions result, for the crash reports

		bool HasText(const Span& a_span, const char* a_text)
		{
			const std::size_t n = std::strlen(a_text);
			if (n == 0 || a_span.text == nullptr || a_span.len < n) {
				return false;
			}
			for (std::size_t i = 0; i + n <= a_span.len; ++i) {
				std::size_t k = 0;
				while (k < n && std::tolower(static_cast<unsigned char>(a_span.text[i + k])) == std::tolower(static_cast<unsigned char>(a_text[k]))) {
					++k;
				}
				if (k == n) {
					return true;
				}
			}
			return false;
		}

		// Hex text with no "0x" and no leading zeros, upper case.
		void NormalHex(const char* a_in, char* a_out, std::size_t a_cap)
		{
			if (a_in[0] == '0' && (a_in[1] == 'x' || a_in[1] == 'X')) {
				a_in += 2;
			}
			while (a_in[0] == '0' && a_in[1] != '\0') {
				++a_in;
			}
			std::size_t i = 0;
			for (; a_in[i] != '\0' && i + 1 < a_cap; ++i) {
				a_out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(a_in[i])));
			}
			a_out[i] = '\0';
		}

		// "nvidiaDriverIssues" -> "nvidia driver"
		void GroupTitle(const char* a_key, char* a_out, std::size_t a_cap)
		{
			std::size_t o = 0;
			for (std::size_t i = 0; a_key[i] != '\0' && o + 2 < a_cap; ++i) {
				const char c = a_key[i];
				if (std::isupper(static_cast<unsigned char>(c)) && i > 0 && !std::isupper(static_cast<unsigned char>(a_key[i - 1]))) {
					a_out[o++] = ' ';
				}
				a_out[o++] = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			a_out[o] = '\0';
			for (const char* end : { " issues", " files", " crash" }) {
				const std::size_t n = std::strlen(end);
				if (o >= n && std::strcmp(a_out + o - n, end) == 0) {
					o -= n;
					a_out[o] = '\0';
				}
			}
		}

		int Compare(const std::uint16_t a_a[4], const std::uint16_t a_b[4])
		{
			for (int i = 0; i < 4; ++i) {
				if (a_a[i] != a_b[i]) {
					return a_a[i] < a_b[i] ? -1 : 1;
				}
			}
			return 0;
		}

		bool IsSet(const std::uint16_t a_v[4]) { return a_v[0] || a_v[1] || a_v[2] || a_v[3]; }

		void ParseVersion(const char* a_text, std::uint16_t a_out[4])
		{
			a_out[0] = a_out[1] = a_out[2] = a_out[3] = 0;
			while (*a_text && !std::isdigit(static_cast<unsigned char>(*a_text))) {
				++a_text;
			}
			for (int i = 0; i < 4 && std::isdigit(static_cast<unsigned char>(*a_text)); ++i) {
				unsigned v = 0;
				while (std::isdigit(static_cast<unsigned char>(*a_text))) {
					v = v * 10 + static_cast<unsigned>(*a_text++ - '0');
				}
				a_out[i] = static_cast<std::uint16_t>(v > 65535 ? 65535 : v);
				if (*a_text != '.') {
					break;
				}
				++a_text;
			}
		}

		bool FileVersion(HMODULE a_module, std::uint16_t a_out[4])
		{
			wchar_t path[MAX_PATH * 2];
			if (GetModuleFileNameW(a_module, path, static_cast<DWORD>(std::size(path))) == 0) {
				return false;
			}
			DWORD       handle = 0;
			const DWORD size = GetFileVersionInfoSizeW(path, &handle);
			if (size == 0) {
				return false;
			}
			static unsigned char data[64 * 1024];
			if (size > sizeof(data) || !GetFileVersionInfoW(path, 0, size, data)) {
				return false;
			}
			VS_FIXEDFILEINFO* info = nullptr;
			UINT              len = 0;
			if (!VerQueryValueW(data, L"\\", reinterpret_cast<void**>(&info), &len) || !info || len < sizeof(VS_FIXEDFILEINFO)) {
				return false;
			}
			a_out[0] = HIWORD(info->dwFileVersionMS);
			a_out[1] = LOWORD(info->dwFileVersionMS);
			a_out[2] = HIWORD(info->dwFileVersionLS);
			a_out[3] = LOWORD(info->dwFileVersionLS);
			return true;
		}

		const char* Explain(const char* a_file)
		{
			for (const auto& e : kPhostwoodExplanations) {
				if (e.file[0] != '\0' && _stricmp(e.file, a_file) == 0) {
					return e.what;
				}
			}
			return nullptr;
		}
	}

	const char* Commit() { return kPhostwoodCommit; }
	const char* Date() { return kPhostwoodDate; }

	int CheckDllVersions(const char* a_gameVersion, Out& a_out)
	{
		std::uint16_t game[4];
		ParseVersion(a_gameVersion, game);
		Out keep{ g_dllText, sizeof(g_dllText), 0 };
		g_dllText[0] = '\0';
		int found = 0;
		for (const auto& r : kPhostwoodDllVersions) {
			if (r.dll == nullptr || r.dll[0] == '\0') {
				continue;
			}
			wchar_t wide[128];
			if (MultiByteToWideChar(CP_UTF8, 0, r.dll, -1, wide, static_cast<int>(std::size(wide))) == 0) {
				continue;
			}
			const HMODULE module = GetModuleHandleW(wide);
			std::uint16_t v[4];
			if (!module || !FileVersion(module, v)) {
				continue;
			}
			// The rules of the analyzer (hasCompatibleDll): older than ignoreBelow, or newer
			// than the listed version: works. Else: works only up to maxGame.
			if (IsSet(r.ignoreBelow) && Compare(v, r.ignoreBelow) < 0) {
				continue;
			}
			if (Compare(v, r.version) > 0 || !IsSet(r.maxGame) || Compare(game, r.maxGame) <= 0) {
				continue;
			}
			++found;
			const bool unknown = v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] <= 1;
			for (Out* o : { &a_out, &keep }) {
				if (unknown) {
					Put(*o, "%s (version not given in the file): ", r.dll);
				} else {
					Put(*o, "%s %u.%u.%u.%u: ", r.dll, unsigned{ v[0] }, unsigned{ v[1] }, unsigned{ v[2] }, unsigned{ v[3] });
				}
				Put(*o, "does not work well with game version %u.%u.%u. Update to %s %s or later (%s).%s%s\n", unsigned{ game[0] }, unsigned{ game[1] }, unsigned{ game[2] }, r.mod,
					r.recommended, r.url, r.note[0] ? " " : "", r.note);
			}
		}
		return found;
	}

	int Append(Out& a_out, const CrashInput& a_in)
	{
		int result = 0;
		Put(a_out, "\nKnown crash signs (data of Phostwood's Crash Log Analyzer, version %s of %s):\n", kPhostwoodCommit[0] ? kPhostwoodCommit : "unknown",
			kPhostwoodDate[0] ? kPhostwoodDate : "unknown date");
		int  signs = 0;
		char gameHex[32] = "";
		if (a_in.gameOffset && a_in.gameOffset[0]) {
			NormalHex(a_in.gameOffset, gameHex, sizeof(gameHex));
		}
		for (const auto& g : kPhostwoodGroups) {
			if (g.key[0] == '\0') {
				continue;
			}
			if (g.requiresModule[0] != '\0') {
				bool loaded = false;
				for (int i = 0; i < a_in.loadedCount && !loaded; ++i) {
					loaded = _stricmp(a_in.loaded[i], g.requiresModule) == 0;
				}
				if (!loaded) {
					continue;
				}
			}
			if (g.requiresText[0] != '\0' && !HasText(a_in.top, g.requiresText) && !HasText(a_in.exception, g.requiresText)) {
				continue;
			}
			char title[96];
			GroupTitle(g.key, title, sizeof(title));

			// The crash address is a known address in the game executable.
			for (int i = 0; i < g.hex && gameHex[0]; ++i) {
				const auto& it = kPhostwoodItems[g.first + g.count + i];
				char        hex[32];
				NormalHex(it.text, hex, sizeof(hex));
				if (std::strcmp(hex, gameHex) == 0) {
					Put(a_out, "  Probable: %s. The crash address is the known address %s (%s).\n", title, it.text, it.what);
					++signs;
				}
			}

			// Names in the exception, call stack or registers: probable. Only in the stack: possible.
			int high = 0, low = 0;
			for (int i = 0; i < g.count; ++i) {
				const auto& it = kPhostwoodItems[g.first + i];
				if (HasText(a_in.exception, it.text) || HasText(a_in.callStackAndRegs, it.text)) {
					++high;
				} else if (HasText(a_in.top, it.text)) {
					++low;
				}
			}
			if (high + low == 0) {
				continue;
			}
			++signs;
			Put(a_out, "  %s: %s. Found: ", high > 0 && !g.possibleOnly ? "Probable" : "Possible", title);
			bool first = true;
			for (int i = 0; i < g.count; ++i) {
				const auto& it = kPhostwoodItems[g.first + i];
				if (!HasText(a_in.exception, it.text) && !HasText(a_in.top, it.text)) {
					continue;
				}
				Put(a_out, "%s%s%s%s%s", first ? "" : "; ", it.text, it.what[0] ? " (" : "", it.what, it.what[0] ? ")" : "");
				first = false;
			}
			Put(a_out, ".\n");
		}
		if (signs == 0) {
			Put(a_out, "  None.\n");
		} else {
			result |= kFoundSigns;
			Put(a_out, "  Probable: found in the exception, the call stack or the registers. Possible: found only in the stack. "
					   "A sign is a place to start, not proof.\n");
		}

		// Short explanations of the files on the call stack.
		bool head = false;
		for (int i = 0; i < a_in.onStackCount; ++i) {
			if (const char* what = Explain(a_in.onStack[i])) {
				if (!head) {
					Put(a_out, "\nFiles on the call stack, explained (Phostwood's data):\n");
					head = true;
				}
				Put(a_out, "  %s: %s\n", a_in.onStack[i], what);
			}
		}

		// Antivirus and overlay programs in the game process.
		head = false;
		const struct
		{
			const PhostwoodProductFile* items;
			std::size_t                 count;
			bool                        antivirus;
		} lists[] = { { kPhostwoodAntivirus, std::size(kPhostwoodAntivirus), true }, { kPhostwoodOverlays, std::size(kPhostwoodOverlays), false } };
		for (const auto& list : lists) {
			const bool antivirus = list.antivirus;
			for (std::size_t n = 0; n < list.count; ++n) {
				const auto& p = list.items[n];
				if (p.file[0] == '\0') {
					continue;
				}
				for (int i = 0; i < a_in.loadedCount; ++i) {
					if (_stricmp(a_in.loaded[i], p.file) != 0) {
						continue;
					}
					if (!head) {
						Put(a_out, "\nPrograms with code in the game process that are known to cause crashes (Phostwood's data):\n");
						head = true;
					}
					result |= kFoundPrograms;
					Put(a_out, "  %s: %s (%s). Add the game folder as an exception, or turn it off for a test.\n", antivirus ? "Antivirus" : "Overlay", p.product,
						p.file);
					break;
				}
			}
		}

		// The DLL version check of the game start.
		if (g_dllText[0] != '\0') {
			Put(a_out, "\nDLLs that do not work well with this game version (Phostwood's data, checked at the game start):\n%s", g_dllText);
			result |= kFoundDllVersion;
		}
		return result;
	}
}
