#pragma once

// From AbyssusLib 2.1.0
// The line above tells which AbyssusLib version this copy comes from. Do not
// change this copy: the build check of each plugin compares it with the file
// of that version (tag v2.1.0) in the AbyssusLib repository.

// AbyssusLib API for other SKSE plugins. Copy this one header into your plugin.
// It links nothing: it finds AbyssusLib.dll at runtime. If AbyssusLib is not
// installed, IsAvailable() is false and the calls do nothing.
//
// Needs CommonLibSSE-NG (it uses REX::W32 to find the DLL), or windows.h
// included before this header (for code without CommonLibSSE).
//
// All functions are safe from all threads.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WINDOWS_)
#	define ABYSSUS_GET_MODULE(a_name) ::GetModuleHandleW(a_name)
#	define ABYSSUS_GET_PROC(a_module, a_name) ::GetProcAddress(a_module, a_name)
#	define ABYSSUS_GET_EXE_PATH(a_buffer, a_size) ::GetModuleFileNameW(nullptr, a_buffer, a_size)
#else
#	include "REX/W32/KERNEL32.h"
#	define ABYSSUS_GET_MODULE(a_name) REX::W32::GetModuleHandleW(a_name)
#	define ABYSSUS_GET_PROC(a_module, a_name) REX::W32::GetProcAddress(a_module, a_name)
#	define ABYSSUS_GET_EXE_PATH(a_buffer, a_size) REX::W32::GetModuleFileNameW(nullptr, a_buffer, a_size)
#endif

// Window data. Plain C types.
//
// Each record starts with its size in bytes (the WindowBuilder sets it). A
// newer AbyssusLib can add fields at the end of a record: it reads only the
// fields that are in the size that the plugin sent, and uses 0 for the
// others. An older AbyssusLib reads only the fields that it knows. Thus old
// and new plugins work with old and new AbyssusLib versions.
// Rule for new fields: add them only at the end, and 0 must mean "default".
extern "C"
{
	// a == 0: the default text color.
	struct AbyssusColor
	{
		float r;
		float g;
		float b;
		float a;
	};

	enum AbyssusItemType
	{
		kAbyssusText = 0,       // text; the next text goes on the same line
		kAbyssusNewLine = 1,    // ends the line
		kAbyssusSeparator = 2,  // a horizontal line
		kAbyssusGraph = 3,      // a line graph of values
	};

	struct AbyssusItem
	{
		int          size;        // sizeof(AbyssusItem) of the plugin
		int          type;        // AbyssusItemType
		AbyssusColor color;       // text and graph color
		const char*  text;        // kAbyssusText
		const float* values;      // kAbyssusGraph
		int          valueCount;  // kAbyssusGraph
		float        graphMin;    // kAbyssusGraph: value at the bottom
		float        graphMax;    // kAbyssusGraph: value at the top
		float        width;       // kAbyssusGraph: size in pixels
		float        height;
	};

	// AbyssusLib 2.0 puts all windows into one stack at the top left; x and y
	// are not used yet (kept for later versions).
	struct AbyssusWindow
	{
		int                size;             // sizeof(AbyssusWindow) of the plugin
		int                itemSize;         // sizeof(AbyssusItem) of the plugin: the step from one item to the next
		float              x;                // not used in 2.0
		float              y;                // not used in 2.0
		const char*        below;            // id of a window of the same source, or nullptr. If that
		                                     // window shows, this window goes below it.
		float              backgroundAlpha;  // 0 to 1
		float              wrapWidth;        // text wrap, pixels from the left window edge; 0 = no wrap
		const AbyssusItem* items;
		int                itemCount;
	};
}

namespace abyssus
{
	inline constexpr int kApiVersion = 2;

	// The folder for all files of a mod: its log, its settings (INI) and its data.
	// <game folder>/Data/SKSE/Plugins/AbyssusLib/<a_mod>/, made if it is not there.
	// All mods that use AbyssusLib keep their files there, one folder for each mod.
	// The header finds the folder itself: it works before AbyssusLib.dll is loaded,
	// and also when AbyssusLib is not installed. With Mod Organizer 2, new files in
	// the Data folder go to the overwrite folder.
	inline std::filesystem::path ModFolder(std::string_view a_mod)
	{
		wchar_t       buffer[1024] = L"";
		const auto    length = ABYSSUS_GET_EXE_PATH(buffer, static_cast<std::uint32_t>(sizeof(buffer) / sizeof(buffer[0])));
		const bool    ok = length > 0 && length < sizeof(buffer) / sizeof(buffer[0]);
		const auto    game = ok ? std::filesystem::path(buffer).parent_path() : std::filesystem::path{};
		const auto    folder = game / "Data" / "SKSE" / "Plugins" / "AbyssusLib" / std::filesystem::path(std::string(a_mod));
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		return folder;
	}

	// A file of an older version of a mod (outside ModFolder): if a_new is not there and
	// a_old is, a_old is copied to a_new (for example the INI file with the user's
	// settings). The old file stays where it is and is no longer used. True if copied.
	inline bool AdoptOldFile(const std::filesystem::path& a_old, const std::filesystem::path& a_new)
	{
		std::error_code ec;
		if (std::filesystem::exists(a_new, ec) || !std::filesystem::exists(a_old, ec))
			return false;
		std::filesystem::create_directories(a_new.parent_path(), ec);
		return std::filesystem::copy_file(a_old, a_new, ec) && !ec;
	}

	enum class Level : int
	{
		kInfo = 0,
		kWarning = 1,
		kError = 2,
	};

	namespace detail
	{
		using VersionFn = int (*)();
		using NotifyFn = void (*)(const char* a_source, const char* a_text, float a_seconds, int a_level);
		using StatusFn = void (*)(const char* a_source, const char* a_text);
		using WindowFn = void (*)(const char* a_source, const char* a_id, const AbyssusWindow* a_window);
		using NoteFn = void (*)(const char* a_source, const char* a_id, const char* a_text, int a_level);
		using HasFn = bool (*)(const char* a_feature);
		using SourceInfoFn = void (*)(const char* a_source, const char* a_group, int a_order);
		using NoteColorFn = void (*)(const char* a_source, const char* a_id, const char* a_text, int a_level, float a_r, float a_g, float a_b, float a_a);

		struct Api
		{
			VersionFn version{ nullptr };
			NotifyFn  notify{ nullptr };
			StatusFn  status{ nullptr };
			WindowFn  window{ nullptr };
			NoteFn    note{ nullptr };
			HasFn        has{ nullptr };
			SourceInfoFn sourceInfo{ nullptr };
			NoteColorFn  noteColor{ nullptr };
			int          dllVersion{ 0 };  // 0 = AbyssusLib is not loaded
		};

		inline const Api& Get()
		{
			static const Api api = [] {
				Api a{};
				if (auto module = ABYSSUS_GET_MODULE(L"AbyssusLib.dll")) {
					a.version = reinterpret_cast<VersionFn>(ABYSSUS_GET_PROC(module, "AbyssusAPI_Version"));
					const int version = a.version ? a.version() : 0;
					a.dllVersion = version;
					a.has = reinterpret_cast<HasFn>(ABYSSUS_GET_PROC(module, "AbyssusAPI_Has"));
					a.sourceInfo = reinterpret_cast<SourceInfoFn>(ABYSSUS_GET_PROC(module, "AbyssusAPI_SetSourceInfo"));
					a.noteColor = reinterpret_cast<NoteColorFn>(ABYSSUS_GET_PROC(module, "AbyssusAPI_SetNoteColor"));
					if (version >= 1) {
						a.notify = reinterpret_cast<NotifyFn>(ABYSSUS_GET_PROC(module, "AbyssusAPI_Notify"));
						a.status = reinterpret_cast<StatusFn>(ABYSSUS_GET_PROC(module, "AbyssusAPI_SetStatus"));
					}
					if (version >= 2) {
						a.window = reinterpret_cast<WindowFn>(ABYSSUS_GET_PROC(module, "AbyssusAPI_SetWindow"));
						a.note = reinterpret_cast<NoteFn>(ABYSSUS_GET_PROC(module, "AbyssusAPI_SetNote"));
					}
				}
				return a;
			}();
			return api;
		}
	}

	// The API version of the loaded AbyssusLib.dll, 0 if it is not loaded.
	inline int DllApiVersion() { return detail::Get().dllVersion; }

	// True if AbyssusLib.dll is loaded and its API version is not the
	// version of this header. Only a signal for the log: use Has() to know
	// what works.
	inline bool VersionMismatch() { return DllApiVersion() != 0 && DllApiVersion() != kApiVersion; }

	// True if the loaded AbyssusLib.dll has a feature, by name:
	//   "notes", "status", "windows", "persistent-notes", "stack", "settings-file",
	//   "source-info", "sized-data", "note-colors"
	// A newer AbyssusLib can have more names. For an older DLL with no
	// AbyssusAPI_Has, the answer comes from the functions that it has.
	inline bool Has(const char* a_feature)
	{
		const auto& api = detail::Get();
		if (api.has)
			return api.has(a_feature);
		const std::string_view name{ a_feature ? a_feature : "" };
		return (name == "notes" && api.notify) || (name == "status" && api.status) || (name == "windows" && api.window) ||
		       (name == "persistent-notes" && api.note);
	}

	// True if AbyssusLib.dll is loaded (notes and status lines). Call it at or
	// after SKSE kPostLoad (all plugins are loaded then).
	inline bool IsAvailable() { return detail::Get().notify != nullptr; }

	// True if AbyssusLib.dll is loaded and has windows (API version 2 or later).
	inline bool HasWindows() { return detail::Get().window != nullptr; }

	// Adds a note to the notification queue for a_seconds. The queue is at
	// the bottom of the stack, oldest note at the top.
	inline void Notify(const char* a_source, const char* a_text, float a_seconds = 5.0f, Level a_level = Level::kInfo)
	{
		if (const auto fn = detail::Get().notify)
			fn(a_source, a_text, a_seconds, static_cast<int>(a_level));
	}

	// A note in the notification queue that stays until it is changed or
	// cleared (no time limit). A change keeps its place in the queue. An
	// empty or null a_text clears it. a_id is unique in a_source.
	inline void SetNote(const char* a_source, const char* a_id, const char* a_text, Level a_level = Level::kInfo)
	{
		if (const auto fn = detail::Get().note)
			fn(a_source, a_id, a_text, static_cast<int>(a_level));
	}

	// The same, with its own color (a == 0: the color of a_level). An AbyssusLib with no
	// "note-colors" uses the color of a_level.
	inline void SetNote(const char* a_source, const char* a_id, const char* a_text, Level a_level, const AbyssusColor& a_color)
	{
		if (const auto fn = detail::Get().noteColor)
			fn(a_source, a_id, a_text, static_cast<int>(a_level), a_color.r, a_color.g, a_color.b, a_color.a);
		else
			SetNote(a_source, a_id, a_text, a_level);
	}

	inline void ClearNote(const char* a_source, const char* a_id) { SetNote(a_source, a_id, nullptr); }

	// A permanent status line for a_source, shown until it is changed or
	// cleared. An empty or null a_text clears it.
	inline void SetStatus(const char* a_source, const char* a_text)
	{
		if (const auto fn = detail::Get().status)
			fn(a_source, a_text);
	}

	// Tells AbyssusLib the group of a_source (a name that the configurations
	// of AbyssusLib.ini use, for example "GPS") and its place in the stack
	// (low = top; plugins with no place: 1000). The [Groups] and [Order]
	// lines of AbyssusLib.ini win over this call. Call it one time, before the
	// first window or status. A plugin with no group is in "Others".
	inline void SetSourceInfo(const char* a_source, const char* a_group, int a_order)
	{
		if (const auto fn = detail::Get().sourceInfo)
			fn(a_source, a_group, a_order);
	}

	// Shows a window, or changes its content. AbyssusLib copies the data: it
	// can be released after the call. The window shows until it is changed or
	// cleared. A null a_window clears it. a_id is unique in a_source.
	// The stack: the plugins in their order (SetSourceInfo, AbyssusLib.ini),
	// then the notes. A change keeps the place.
	inline void SetWindow(const char* a_source, const char* a_id, const AbyssusWindow* a_window)
	{
		if (const auto fn = detail::Get().window)
			fn(a_source, a_id, a_window);
	}

	inline void ClearWindow(const char* a_source, const char* a_id) { SetWindow(a_source, a_id, nullptr); }

	// Makes the content of a window. Use one builder for each window and
	// call Clear() before each new content.
	class WindowBuilder
	{
	public:
		static constexpr AbyssusColor kDefault{ 0.0f, 0.0f, 0.0f, 0.0f };

		void Clear()
		{
			_items.clear();
			_texts.clear();
			_values.clear();
		}

		// Text; the next text goes on the same line.
		void Text(const AbyssusColor& a_color, const char* a_fmt, ...)
		{
			char    text[512];
			va_list args;
			va_start(args, a_fmt);
			std::vsnprintf(text, sizeof(text), a_fmt, args);
			va_end(args);
			_texts.emplace_back(text);
			Add(kAbyssusText, a_color).text = _texts.back().c_str();
		}

		void NewLine() { Add(kAbyssusNewLine, kDefault); }

		void Separator() { Add(kAbyssusSeparator, kDefault); }

		void Graph(const float* a_values, int a_count, float a_min, float a_max, float a_width, float a_height, const AbyssusColor& a_color = kDefault)
		{
			_values.emplace_back(a_values, a_values + (a_count > 0 ? a_count : 0));
			auto& item = Add(kAbyssusGraph, a_color);
			item.values = _values.back().data();
			item.valueCount = static_cast<int>(_values.back().size());
			item.graphMin = a_min;
			item.graphMax = a_max;
			item.width = a_width;
			item.height = a_height;
		}

		bool Empty() const { return _items.empty(); }

		// The window data. It is valid until the next change of the builder.
		AbyssusWindow Make(float a_backgroundAlpha = 0.6f, float a_wrapWidth = 0.0f, const char* a_below = nullptr) const
		{
			return AbyssusWindow{ static_cast<int>(sizeof(AbyssusWindow)), static_cast<int>(sizeof(AbyssusItem)), 0.0f, 0.0f, a_below, a_backgroundAlpha,
				a_wrapWidth, _items.data(), static_cast<int>(_items.size()) };
		}

	private:
		AbyssusItem& Add(int a_type, const AbyssusColor& a_color)
		{
			AbyssusItem item{};
			item.size = static_cast<int>(sizeof(AbyssusItem));
			item.type = a_type;
			item.color = a_color;
			_items.push_back(item);
			return _items.back();
		}

		std::vector<AbyssusItem>        _items;
		std::deque<std::string>         _texts;   // deque: the c_str() pointers stay valid when more are added
		std::deque<std::vector<float>>  _values;
	};
}

#undef ABYSSUS_GET_MODULE
#undef ABYSSUS_GET_PROC
#undef ABYSSUS_GET_EXE_PATH
