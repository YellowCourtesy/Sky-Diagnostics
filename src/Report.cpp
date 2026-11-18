#include "Report.h"

#include "Config.h"
#include "ModMap.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>

namespace skydiag::report
{
	namespace
	{
		struct EventLine
		{
			int         second;
			std::string text;
			std::string cell;  // the place of the player at the event
		};

		std::string g_cell;

		// A loading screen: start and end in seconds from the session start.
		// The order in this list is the order of the "Loading screen:" events,
		// so the numbers in the graphs and in the table are the same.
		struct LoadSpan
		{
			float start;
			float end;
		};
		std::vector<LoadSpan> g_loads;

		std::filesystem::path    g_baseDir;
		std::filesystem::path    g_configFile;
		std::string              g_group;     // character name, empty = no save loaded yet
		std::vector<std::string> g_problems;  // problem ids that occurred, in order

		// Processor load, added up over the session.
		struct LoadRow
		{
			std::string name;
			std::string role;  // thread rows only
			double      sumA{ 0 };  // mods: own code. scripts: running. threads: cores
			double      sumB{ 0 };  // mods: waiting. scripts: waiting
			double      sumC{ 0 };  // mods: total
			float       peakA{ 0 };
		};
		std::vector<LoadRow> g_modRows;
		std::vector<LoadRow> g_scriptRows;
		std::vector<LoadRow> g_threadRows;
		std::vector<LoadRow> g_functionRows;  // sumA: running
		std::vector<LoadRow> g_timeRows;      // sumA: ms each second, sumB: calls each second
		std::vector<LoadRow> g_memoryRows;    // sumA: in use now, sumB: all allocations, peakA: largest in use

		// A table cell with the mod of a file, if the mod manager data is loaded.
		std::string ModCell(const char* a_mod);
		int                  g_loadPeriods = 0;

		LoadRow& Row(std::vector<LoadRow>& a_rows, const char* a_name, const char* a_role)
		{
			for (auto& row : a_rows) {
				if (row.name == a_name && row.role == a_role) {
					return row;
				}
			}
			a_rows.push_back({ a_name, a_role });
			return a_rows.back();
		}

		// A name that is safe as a folder name.
		std::string SafeName(const char* a_name)
		{
			std::string out;
			for (const char* p = a_name; p && *p && out.size() < 60; ++p) {
				const unsigned char c = static_cast<unsigned char>(*p);
				out += (std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c >= 0x80) ? *p : '_';
			}
			while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
				out.pop_back();
			}
			return out.empty() ? "Unnamed" : out;
		}

		// The text for a problem id in the report.
		const char* ProblemTitle(const std::string& a_id)
		{
			static const std::pair<const char*, const char*> titles[] = {
				{ "fps-cpu", "Low frame rate, the processor is the limit" },
				{ "fps-gpu", "Low frame rate, the GPU is the limit" },
				{ "fps-cap", "Frame rate held by a frame cap or VSync" },
				{ "stutter", "Stutter (single slow frames)" },
				{ "latency", "High input latency" },
				{ "memory-low", "Memory is low" },
				{ "memory-full", "Memory is almost full" },
				{ "memory-growth", "Game memory grows (possible leak)" },
				{ "vram-high", "Video memory is almost full" },
				{ "vram-full", "Video memory is full" },
				{ "papyrus-stacks", "Papyrus has too many script stacks" },
				{ "references", "Object references near the limit" },
				{ "plugins", "Plugin count near or over the limit" },
				{ "stall", "Freeze or very long load" },
			};
			for (const auto& [id, title] : titles) {
				if (a_id == id) {
					return title;
				}
			}
			return nullptr;
		}

		std::mutex               g_lock;
		std::filesystem::path    g_dir;
		Info                     g_info;
		std::string              g_name;       // "session-YYYYMMDD-HHMMSS"
		std::string              g_startText;  // "YYYY-MM-DD HH:MM:SS"
		std::vector<Sample>      g_samples;
		std::vector<EventLine>   g_events;
		std::vector<Summary>     g_previous;
		bool                     g_started = false;
		bool                     g_compared = false;

		void LoadPrevious()
		{
			g_previous.clear();
			std::error_code ec;
			for (const auto& entry : std::filesystem::directory_iterator(g_dir, ec)) {
				if (entry.path().extension() != ".sum" || entry.path().stem().string() == g_name) {
					continue;
				}
				std::ifstream     file(entry.path());
				std::stringstream text;
				text << file.rdbuf();
				Summary summary;
				if (ParseSummary(text.str(), summary)) {
					summary.name = entry.path().stem().string();
					g_previous.push_back(summary);
				}
			}
		}

		std::string Fmt(const char* a_fmt, ...)
		{
			char    buffer[1024];
			va_list args;
			va_start(args, a_fmt);
			std::vsnprintf(buffer, sizeof(buffer), a_fmt, args);
			va_end(args);
			return buffer;
		}

		std::tm LocalTime(std::time_t a_time)
		{
			std::tm result{};
#ifdef _WIN32
			localtime_s(&result, &a_time);
#else
			localtime_r(&a_time, &result);
#endif
			return result;
		}

		std::string Escape(const std::string& a_text)
		{
			std::string out;
			out.reserve(a_text.size());
			for (const char c : a_text) {
				switch (c) {
				case '&': out += "&amp;"; break;
				case '<': out += "&lt;"; break;
				case '>': out += "&gt;"; break;
				case '"': out += "&quot;"; break;
				default: out += c; break;
				}
			}
			return out;
		}

		std::string ModCell(const char* a_mod)
		{
			return a_mod && a_mod[0] ? "<td>" + Escape(a_mod) + "</td>" : std::string("<td></td>");
		}

		std::string Clock(int a_second)
		{
			return Fmt("%d:%02d:%02d", a_second / 3600, (a_second / 60) % 60, a_second % 60);
		}

		bool Counts(const Sample& a_sample)
		{
			return !a_sample.loading && a_sample.active && a_sample.fps > 1.0f;
		}

		bool IsSlow(const Sample& a_sample)
		{
			return a_sample.worstFrameMs >= 50.0f && a_sample.worstFrameMs >= 3000.0f / a_sample.fps;
		}

		// The class of an event line, for the color. The colors are the same
		// as in the game.
		const char* EventClass(const std::string& a_text)
		{
			const auto starts = [&](const char* a_prefix) { return a_text.rfind(a_prefix, 0) == 0; };
			if (starts("Critical:")) {
				return "critical";
			}
			if (starts("Warning:") || starts("Stall:")) {
				return "warn";
			}
			if (starts("Note:")) {
				return "note";
			}
			if (starts("Comparison:")) {
				return "info";
			}
			return "";
		}

		bool IsWarningLine(const std::string& a_text)
		{
			const char* cls = EventClass(a_text);
			return cls[0] != '\0' && std::string(cls) != "info";
		}

		// The frame cap, measured: the FPS value that the game holds for a
		// large part of the time. 0 if there is no clear value.
		float MeasuredFrameCap(const std::vector<Sample>& a_samples)
		{
			std::vector<int> rounded;
			for (const auto& sample : a_samples) {
				if (Counts(sample)) {
					rounded.push_back(static_cast<int>(sample.fps + 0.5f));
				}
			}
			if (rounded.size() < 30) {
				return 0.0f;
			}
			std::sort(rounded.begin(), rounded.end());
			// A cap is the highest value that the game reaches, and the game
			// is near it for 40 % of the time or more.
			const int top = rounded[rounded.size() * 98 / 100];
			int       near = 0;
			for (const int value : rounded) {
				if (value >= top * 0.97f) {
					++near;
				}
			}
			return near * 100 >= static_cast<int>(rounded.size()) * 40 ? static_cast<float>(top) : 0.0f;
		}

		// A line graph as inline SVG. The values are averaged into at most
		// 900 points.
		//
		// The scale comes from the seconds in which the game ran normally.
		// The game start and the loading screens have values that are much
		// larger, and they would make the rest of the line flat. Such values
		// are cut at the top of the graph.
		//
		// a_limit is the largest possible value (0 = not known). If the line
		// comes within 10 % of it, the graph shows the limit as a line. If
		// not, the caller puts the limit in the title.
		template <class Get>
		std::string Graph(const std::vector<Sample>& a_samples, const std::string& a_title, const char* a_unit, float a_limit, Get a_get)
		{
			constexpr int width = 900;
			constexpr int height = 300;
			constexpr int left = 60;
			const int     count = static_cast<int>(a_samples.size());
			if (count < 2) {
				return {};
			}

			float peak = 0.0f;
			for (int i = kSkipSeconds; i < count; ++i) {
				if (Counts(a_samples[i])) {
					peak = std::max(peak, a_get(a_samples[i]));
				}
			}
			if (peak <= 0.0f) {
				// No normal seconds yet: use all values.
				for (const auto& sample : a_samples) {
					peak = std::max(peak, a_get(sample));
				}
			}
			const bool showLimit = a_limit > 0.0f && peak >= a_limit * 0.9f;
			peak = std::max(peak, 1.0f);
			// The numbered lines go from zero to the largest value. The plot
			// area is 10 % higher than that, so the line is easy to see at
			// its largest value. The top edge has no number.
			const float top = std::max(showLimit ? std::max(a_limit, peak) : peak, 1.0f) * 1.1f;

			const int          points = std::min(count, width);
			std::vector<float> values(points);
			bool               cut = false;
			for (int p = 0; p < points; ++p) {
				const int from = static_cast<int>(static_cast<long long>(p) * count / points);
				const int to = std::max(from + 1, static_cast<int>(static_cast<long long>(p + 1) * count / points));
				float     sum = 0.0f;
				for (int i = from; i < to; ++i) {
					sum += a_get(a_samples[i]);
				}
				values[p] = sum / static_cast<float>(to - from);
				cut = cut || values[p] > top;
			}

			const auto xOf = [&](float a_second) {
				const float clamped = std::min(std::max(a_second, 0.0f), static_cast<float>(count - 1));
				return static_cast<float>(left) + clamped * static_cast<float>(width) / static_cast<float>(count - 1);
			};
			const auto yOf = [&](float a_value) { return static_cast<float>(height) - std::min(a_value, top) / top * static_cast<float>(height); };

			std::string svg = Fmt("<h3>%s</h3><svg viewBox=\"0 0 %d %d\" width=\"100%%\" role=\"img\">", Escape(a_title).c_str(), width + left + 10, height + 36);
			svg += Fmt("<rect x=\"%d\" y=\"0\" width=\"%d\" height=\"%d\" class=\"plot\"/>", left, width, height);

			// Each loading screen: a transparent band from its start to its
			// end, a line at each side, and its number below the start.
			for (std::size_t i = 0; i < g_loads.size(); ++i) {
				const float x1 = xOf(g_loads[i].start);
				const float x2 = std::max(xOf(g_loads[i].end), x1 + 1.0f);
				svg += Fmt("<rect x=\"%.1f\" y=\"0\" width=\"%.1f\" height=\"%d\" class=\"loadspan\"/>", static_cast<double>(x1), static_cast<double>(x2 - x1), height);
				svg += Fmt("<line x1=\"%.1f\" y1=\"0\" x2=\"%.1f\" y2=\"%d\" class=\"load\"/>", static_cast<double>(x1), static_cast<double>(x1), height);
				svg += Fmt("<line x1=\"%.1f\" y1=\"0\" x2=\"%.1f\" y2=\"%d\" class=\"load\"/>", static_cast<double>(x2), static_cast<double>(x2), height);
				svg += Fmt("<text x=\"%.1f\" y=\"%d\" class=\"loadnum\" text-anchor=\"middle\">%d</text>", static_cast<double>(x1), height + 12, static_cast<int>(i) + 1);
			}

			// Five numbered lines, from zero to the largest value. Values
			// with a decimal part are rounded up.
			for (int line = 0; line <= 4; ++line) {
				const float value = peak * static_cast<float>(line) / 4.0f;
				const float y = yOf(value);
				if (line > 0) {
					svg += Fmt("<line x1=\"%d\" y1=\"%.1f\" x2=\"%d\" y2=\"%.1f\" class=\"grid\"/>", left, static_cast<double>(y), width + left, static_cast<double>(y));
				}
				svg += Fmt("<text x=\"%d\" y=\"%.1f\" class=\"axis\" text-anchor=\"end\">%.0f</text>", left - 4, static_cast<double>(line == 0 ? y : y + 4.0f),
					static_cast<double>(std::ceil(value)));
			}
			if (showLimit) {
				const float y = yOf(a_limit);
				svg += Fmt("<line x1=\"%d\" y1=\"%.1f\" x2=\"%d\" y2=\"%.1f\" class=\"limit\"/>", left, static_cast<double>(y), width + left, static_cast<double>(y));
				svg += Fmt("<text x=\"%d\" y=\"%.1f\" class=\"limittext\" text-anchor=\"end\">maximum %s</text>", width + left - 4, static_cast<double>(y) + 13.0,
					std::string(a_unit) == "MB" ? MemoryText(static_cast<std::uint64_t>(a_limit)).c_str() : Fmt("%.0f %s", static_cast<double>(a_limit), a_unit).c_str());
			}
			svg += Fmt("<text x=\"%d\" y=\"%d\" class=\"axis\">0:00:00</text>", left, height + 30);
			svg += Fmt("<text x=\"%d\" y=\"%d\" class=\"axis\" text-anchor=\"end\">%s (%s)</text>", width + left, height + 30, Clock(count).c_str(), a_unit);
			svg += "<polyline class=\"series\" points=\"";
			for (int p = 0; p < points; ++p) {
				const float x = static_cast<float>(left) + static_cast<float>(p) * static_cast<float>(width) / static_cast<float>(points - 1);
				svg += Fmt("%.1f,%.1f ", static_cast<double>(x), static_cast<double>(yOf(values[p])));
			}
			svg += "\"/></svg>\n";
			if (cut) {
				svg += "<p class=\"small\">Values above the scale during loading screens are cut off.</p>\n";
			}
			return svg;
		}

		// "Title (maximum N unit)" if the limit is known and the graph does
		// not show it as a line.
		std::string GraphTitle(const std::vector<Sample>& a_samples, const char* a_title, const char* a_limitName, float a_limit, const char* a_unit,
			float (*a_get)(const Sample&))
		{
			if (a_limit <= 0.0f) {
				return a_title;
			}
			float peak = 0.0f;
			for (int i = kSkipSeconds; i < static_cast<int>(a_samples.size()); ++i) {
				if (Counts(a_samples[i])) {
					peak = std::max(peak, a_get(a_samples[i]));
				}
			}
			if (peak >= a_limit * 0.9f) {
				return a_title;  // the graph shows the limit
			}
			if (std::string(a_unit) == "MB") {
				return Fmt("%s (%s %s)", a_title, a_limitName, MemoryText(static_cast<std::uint64_t>(a_limit)).c_str());
			}
			return Fmt("%s (%s %.0f %s)", a_title, a_limitName, static_cast<double>(a_limit), a_unit);
		}

		std::string MetricsRow(const char* a_title, const Metrics& a_metrics)
		{
			if (a_metrics.seconds == 0) {
				return Fmt("<tr><td>%s</td><td colspan=\"5\">no data</td></tr>\n", a_title);
			}
			return Fmt("<tr><td>%s</td><td>%.1f</td><td>%.1f</td><td>%.1f</td><td>%.1f ms</td><td>%d min</td></tr>\n", a_title,
				static_cast<double>(a_metrics.averageFps), static_cast<double>(a_metrics.lowFps), static_cast<double>(a_metrics.slowPerMinute),
				static_cast<double>(a_metrics.averageWorstMs), a_metrics.seconds / 60);
		}

		Summary CurrentSummary()
		{
			Summary summary;
			summary.name = g_name;
			summary.durationSeconds = static_cast<int>(g_samples.size());
			summary.first = ComputeMetrics(g_samples, kSkipSeconds, kCompareSeconds);
			summary.total = ComputeMetrics(g_samples, kSkipSeconds, static_cast<int>(g_samples.size()));
			return summary;
		}

		bool Qualifies(const Summary& a_summary)
		{
			return a_summary.durationSeconds >= kCompareSeconds && a_summary.first.seconds >= kCompareSeconds / 2;
		}

		std::string BuildHtml()
		{
			const Summary summary = CurrentSummary();
			const int     duration = summary.durationSeconds;

			std::uint32_t peakGame = 0, peakVram = 0;
			for (const auto& sample : g_samples) {
				peakGame = std::max(peakGame, sample.gameMb);
				peakVram = std::max(peakVram, sample.vramMb);
			}

			std::string html;
			html += "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n";
			html += Fmt("<title>SkyrimDiagnostics session %s</title>\n", Escape(g_startText).c_str());
			html += "<style>\n"
					"body{font-family:system-ui,sans-serif;margin:24px auto;max-width:1000px;padding:0 16px;background:#14171a;color:#e2e6ea}\n"
					"h1{font-size:22px}h2{font-size:18px;margin-top:32px;border-bottom:1px solid #3a4046;padding-bottom:4px}h3{font-size:14px;margin:18px 0 4px}\n"
					"table{border-collapse:collapse;width:100%}td,th{padding:4px 10px;border-bottom:1px solid #2c3136;text-align:left;font-size:14px}\n"
					"li{font-size:14px;margin:2px 0}th{color:#9aa4ad;font-weight:600}.plot{fill:#1c2024;stroke:#3a4046}.grid{stroke:#2c3136}.axis{fill:#9aa4ad;font-size:11px}\n"
					".series{fill:none;stroke:#6cb6ff;stroke-width:1.5}.time{color:#9aa4ad;white-space:nowrap}p{font-size:14px;color:#b8c0c8}\n"
					".loadspan{fill:#e0913a;fill-opacity:0.18}.loadnum{fill:#e0913a;font-size:11px}"
					".load{stroke:#e0913a;stroke-width:1;stroke-dasharray:4 3}.limit{stroke:#ff5a4d;stroke-width:1;stroke-dasharray:6 4}.limittext{fill:#ff5a4d;font-size:11px}\n"
					".critical{color:#ff5a4d}.warn{color:#ffcc44}.note{color:#ccd9e6}.info{color:#8cd9ff}.small{font-size:12px;margin:2px 0 0}.cell{color:#9aa4ad}\n"
					"</style></head><body>\n";
			html += Fmt("<h1>SkyrimDiagnostics session report</h1>\n<p>%s, %s, tool version %s<br>Session start: %s. Length: %s.<br>Character: %s</p>\n",
				Escape(g_info.game).c_str(), Escape(g_info.platform).c_str(), Escape(g_info.version).c_str(), Escape(g_startText).c_str(),
				Clock(duration).c_str(),
				g_group.empty() ? "no save was loaded" : Escape(g_group + ". The comparison uses only earlier sessions of this character.").c_str());
			if (modmap::Loaded()) {
				html += Fmt("<p>Mod Organizer 2 data: %d files of %d mods. The tables show the mod of a DLL or a script where it is known.</p>\n",
					modmap::FileCount(), modmap::ModCount());
			} else {
				html += "<p>Mod Organizer 2 data: not found. The tables show file names only.</p>\n";
			}

			html += "<h2>Performance</h2>\n<p>Only seconds in which the game ran normally count: no loading screen, game window active, frames shown. "
					"The first 30 seconds do not count. A slow frame is 50 ms or more and 3 times the average frame time.</p>\n";
			html += "<table><tr><th>Period</th><th>Average FPS</th><th>Low FPS (95 % of the seconds are better)</th><th>Seconds with a slow frame, each minute</th>"
					"<th>Average of the slowest frame each second</th><th>Time that counts</th></tr>\n";
			html += MetricsRow("This session, first 10 minutes", summary.first);
			html += MetricsRow("This session, all", summary.total);
			int   compared = 0;
			float previousFps = 0, previousLow = 0, previousSlow = 0, previousWorst = 0;
			for (const auto& previous : g_previous) {
				if (Qualifies(previous)) {
					++compared;
					previousFps += previous.first.averageFps;
					previousLow += previous.first.lowFps;
					previousSlow += previous.first.slowPerMinute;
					previousWorst += previous.first.averageWorstMs;
				}
			}
			if (compared > 0) {
				Metrics average;
				average.seconds = kCompareSeconds;
				average.averageFps = previousFps / compared;
				average.lowFps = previousLow / compared;
				average.slowPerMinute = previousSlow / compared;
				average.averageWorstMs = previousWorst / compared;
				html += MetricsRow(Fmt("Average of %d earlier session%s, first 10 minutes", compared, compared == 1 ? "" : "s").c_str(), average);
			}
			html += "</table>\n";
			html += Fmt("<p>Largest game memory: %s. Largest video memory: %s.</p>\n", MemoryText(peakGame).c_str(), MemoryText(peakVram).c_str());

			// The largest possible values. Memory and video memory limits
			// come from the last sample. The frame cap is measured.
			const float frameCap = MeasuredFrameCap(g_samples);
			const float gameMax = g_samples.empty() ? 0.0f : static_cast<float>(g_samples.back().gameMaxMb);
			const float vramMax = g_samples.empty() ? 0.0f : static_cast<float>(g_samples.back().vramBudgetMb);
			const auto  fps = [](const Sample& s) { return s.fps; };
			const auto  game = [](const Sample& s) { return static_cast<float>(s.gameMb); };
			const auto  vram = [](const Sample& s) { return static_cast<float>(s.vramMb); };

			html += "<h2>Graphs</h2>\n<p>The scale of each graph comes from the seconds in which the game ran normally. Orange bands: loading screens, from start to end. The number below a band is the number of the loading screen in the table below. "
					"Red dashed line: the maximum of that value, for example the video memory that the system gives to the game. The line is in the graph only "
					"when the value reached 90 % of the maximum. If not, the maximum is in the title of the graph.</p>\n";
			html += Graph(g_samples, frameCap > 0.0f ? Fmt("Frames each second (frame cap, measured: about %.0f FPS)", static_cast<double>(frameCap)) : "Frames each second",
				"FPS", 0.0f, fps);
			html += Graph(g_samples, "Slowest frame of each second", "ms", 0.0f, [](const Sample& s) { return s.worstFrameMs; });
			html += Graph(g_samples, GraphTitle(g_samples, "Game memory", "maximum", gameMax, "MB", game), "MB", gameMax, game);
			html += Graph(g_samples, GraphTitle(g_samples, "Video memory", "maximum", vramMax, "MB", vram), "MB", vramMax, vram);
			html += Graph(g_samples, "Papyrus script stacks that run", "stacks", 0.0f, [](const Sample& s) { return s.papyrusRunning; });
			html += Graph(g_samples, "Draw calls in each frame", "draw calls", 0.0f, [](const Sample& s) { return static_cast<float>(s.drawCalls); });
			if (!g_samples.empty() && g_samples.back().animations != 0) {
				html += Graph(g_samples, "Loaded animation files", "animations", 0.0f, [](const Sample& s) { return static_cast<float>(s.animations); });
			}
			const float referenceMax = g_samples.empty() ? 0.0f : static_cast<float>(g_samples.back().referenceLimit);
			if (referenceMax > 0.0f) {
				const auto references = [](const Sample& s) { return static_cast<float>(s.references); };
				html += Graph(g_samples, GraphTitle(g_samples, "Object references in use", "maximum", referenceMax, "references", references), "references",
					referenceMax, references);
			}

			const auto eventRow = [](const EventLine& a_event) {
				const char* cls = EventClass(a_event.text);
				return Fmt("<tr><td class=\"time\">%s</td><td%s%s%s>%s</td><td class=\"cell\">%s</td></tr>\n", Clock(a_event.second).c_str(),
					cls[0] ? " class=\"" : "", cls, cls[0] ? "\"" : "", Escape(a_event.text).c_str(), Escape(a_event.cell).c_str());
			};
			// a_numbered: the rows get a number in the first column.
			const auto eventList = [&](const char* a_title, bool (*a_match)(const std::string&), bool a_numbered) {
				std::string rows;
				int         number = 0;
				for (const auto& event : g_events) {
					if (!a_match(event.text)) {
						continue;
					}
					std::string line = eventRow(event);
					if (a_numbered) {
						line.insert(4, Fmt("<td class=\"loadnum\">%d</td>", ++number));  // after "<tr>"
					}
					rows += line;
				}
				html += Fmt("<h2>%s</h2>\n", a_title);
				html += rows.empty() ? "<p>None.</p>\n" :
				                       Fmt("<table><tr>%s<th>Time</th><th>Event</th><th>Place of the player</th></tr>\n", a_numbered ? "<th>No.</th>" : "") + rows + "</table>\n";
			};
			eventList("Warnings", &IsWarningLine, false);
			eventList("Loading screens", [](const std::string& a_text) { return a_text.rfind("Loading screen:", 0) == 0; }, true);
			eventList("Slow frames", [](const std::string& a_text) { return a_text.rfind("Slow frame:", 0) == 0; }, false);

			// Processor time: mod DLLs, scripts, threads.
			html += "<h2>Processor time in this session</h2>\n";
			if (g_loadPeriods == 0) {
				html += "<p>No profiler data.</p>\n";
			} else {
				const double periods = static_cast<double>(g_loadPeriods);
				const auto   sorted = [](std::vector<LoadRow> a_rows) {
					std::sort(a_rows.begin(), a_rows.end(), [](const LoadRow& a_lhs, const LoadRow& a_rhs) { return a_lhs.sumA + a_lhs.sumC > a_rhs.sumA + a_rhs.sumC; });
					return a_rows;
				};
				html += "<h3>Mod DLLs</h3>\n<p>In percent of one processor core. Own code: the code of the DLL itself, the real cost of the mod. "
						"Total: includes the game and system code that the DLL calls or wraps with a hook.</p>\n";
				if (g_modRows.empty()) {
					html += "<p>No mod DLL was on the call stacks.</p>\n";
				} else {
					html += "<table><tr><th>Mod DLL</th><th>Mod</th><th>Own code, average</th><th>Own code, largest</th><th>Waiting, average</th><th>Total, average</th></tr>\n";
					int shown = 0;
					for (const auto& row : sorted(g_modRows)) {
						if (++shown > 20) {
							break;
						}
						html += Fmt("<tr><td>%s</td>%s<td>%.1f %%</td><td>%.1f %%</td><td>%.1f %%</td><td>%.1f %%</td></tr>\n", Escape(row.name).c_str(),
							ModCell(modmap::ModForDll(row.name.c_str())).c_str(), row.sumA / periods * 100.0, static_cast<double>(row.peakA) * 100.0, row.sumB / periods * 100.0, row.sumC / periods * 100.0);
					}
					html += "</table>\n";
				}

				html += "<h3>Papyrus scripts</h3>\n<p>The scripts with the most stacks that run. The Mod column shows the mod that has the script as a loose file "
						"(from the Mod Organizer 2 data). A script with no mod is from the game or from an archive (BSA) of a mod.</p>\n";
				if (g_scriptRows.empty()) {
					html += "<p>No script data.</p>\n";
				} else {
					html += "<table><tr><th>Script</th><th>Mod</th><th>Running stacks, average</th><th>Running stacks, largest</th><th>Waiting stacks, average</th></tr>\n";
					int shown = 0;
					for (const auto& row : sorted(g_scriptRows)) {
						if (++shown > 20) {
							break;
						}
						html += Fmt("<tr><td>%s</td>%s<td>%.2f</td><td>%.1f</td><td>%.2f</td></tr>\n", Escape(row.name).c_str(),
							ModCell(modmap::ModForScript(row.name.c_str())).c_str(), row.sumA / periods, static_cast<double>(row.peakA), row.sumB / periods);
					}
					html += "</table>\n";
				}
				if (!g_timeRows.empty()) {
					html += "<h3>Papyrus function time</h3>\n<p>The time that the script engine used in the code of each script function (measured). "
							"1000 ms each second is one processor core. The time of native functions (functions of the game or of an SKSE plugin) "
							"is not included.</p>\n"
							"<table><tr><th>Function</th><th>Mod</th><th>ms each second, average</th><th>ms each second, largest</th><th>Calls each second, average</th></tr>\n";
					int shown = 0;
					for (const auto& row : sorted(g_timeRows)) {
						if (++shown > 25) {
							break;
						}
						const std::string script = row.name.substr(0, row.name.find('.'));
						html += Fmt("<tr><td>%s</td>%s<td>%.2f</td><td>%.1f</td><td>%.0f</td></tr>\n", Escape(row.name).c_str(),
							ModCell(modmap::ModForScript(script.c_str())).c_str(), row.sumA / periods, static_cast<double>(row.peakA), row.sumB / periods);
					}
					html += "</table>\n";
				} else if (!g_functionRows.empty()) {
					html += "<h3>Papyrus functions</h3>\n<p>The functions that were at the top of the stacks that run. A function with a large value has much work "
							"or is called often. The values are from samples (20 each second), not from a time measurement.</p>\n"
							"<table><tr><th>Function</th><th>Mod</th><th>Running stacks, average</th><th>Running stacks, largest</th></tr>\n";
					int shown = 0;
					for (const auto& row : sorted(g_functionRows)) {
						if (++shown > 20) {
							break;
						}
						const std::string script = row.name.substr(0, row.name.find('.'));
						html += Fmt("<tr><td>%s</td>%s<td>%.2f</td><td>%.1f</td></tr>\n", Escape(row.name).c_str(),
							ModCell(modmap::ModForScript(script.c_str())).c_str(), row.sumA / periods, static_cast<double>(row.peakA));
					}
					html += "</table>\n";
				}

				html += "<h3>Threads</h3>\n<p>Where the busy threads of the game worked, by module, in percent of one processor core (average of the session).</p>\n";
				if (g_threadRows.empty()) {
					html += "<p>No thread data.</p>\n";
				} else {
					html += "<table><tr><th>Threads</th><th>Module</th><th>Average</th><th>Largest</th></tr>\n";
					int shown = 0;
					for (const auto& row : sorted(g_threadRows)) {
						if (++shown > 25 || row.sumA / periods < 0.005) {
							break;
						}
						html += Fmt("<tr><td>%s</td><td>%s</td><td>%.1f %%</td><td>%.1f %%</td></tr>\n", Escape(row.role).c_str(), Escape(row.name).c_str(),
							row.sumA / periods * 100.0, static_cast<double>(row.peakA) * 100.0);
					}
					html += "</table>\n";
				}
			}

			// Heap memory by mod DLL.
			if (!g_memoryRows.empty()) {
				html += "<h2>Heap memory by mod DLL</h2>\n<p>Heap memory that each mod DLL allocated and did not release. Not included: memory from the game's "
						"own memory manager (many SKSE plugins use it for game objects), VirtualAlloc, and memory of the graphics driver. "
						"A value that only grows during the session can be a memory leak.</p>\n"
						"<table><tr><th>Mod DLL</th><th>Mod</th><th>In use at the end</th><th>Largest in use</th><th>All allocations</th></tr>\n";
				auto rows = g_memoryRows;
				std::sort(rows.begin(), rows.end(), [](const LoadRow& a_lhs, const LoadRow& a_rhs) { return a_lhs.peakA > a_rhs.peakA; });
				const auto megabytes = [](double a_value) {
					return a_value >= 1024.0 ? Fmt("%.0f MB (%.1f GB)", a_value, a_value / 1024.0) : Fmt("%.1f MB", a_value);
				};
				int shown = 0;
				for (const auto& row : rows) {
					if (++shown > 25) {
						break;
					}
					html += Fmt("<tr><td>%s</td>%s<td>%s</td><td>%s</td><td>%s</td></tr>\n", Escape(row.name).c_str(),
						ModCell(modmap::ModForDll(row.name.c_str())).c_str(), megabytes(row.sumA).c_str(), megabytes(static_cast<double>(row.peakA)).c_str(),
						megabytes(row.sumB).c_str());
				}
				html += "</table>\n";
			}

			// Suggestions for the problem types that occurred.
			html += "<h2>Suggestions</h2>\n";
			if (g_problems.empty()) {
				html += "<p>No problem type occurred in this session.</p>\n";
			} else {
				// The suggestions come from the configuration file, which the
				// user can change while the game runs.
				const auto suggestions = ParseSuggestions(config::Load(g_configFile).suggestions);
				html += Fmt("<p>The lists come from the [[Suggestions]] part of the file %s. You can change that file; the report uses the new text at "
							"its next update. The tool does not check that a mod is correct for your game version.</p>\n",
					Escape(g_configFile.filename().string()).c_str());
				for (const auto& problem : g_problems) {
					const char* title = ProblemTitle(problem);
					html += Fmt("<h3>%s [%s]</h3>\n", Escape(title ? title : problem).c_str(), Escape(problem).c_str());
					const std::vector<std::string>* lines = nullptr;
					for (const auto& [id, list] : suggestions) {
						if (id == problem) {
							lines = &list;
						}
					}
					if (!lines || lines->empty()) {
						html += "<p>No suggestions in the file for this problem type.</p>\n";
						continue;
					}
					html += "<ul>\n";
					for (const auto& line : *lines) {
						html += Fmt("<li>%s</li>\n", Escape(line).c_str());
					}
					html += "</ul>\n";
				}
			}

			html += "<h2>All events</h2>\n<table><tr><th>Time</th><th>Event</th><th>Place of the player</th></tr>\n";
			for (const auto& event : g_events) {
				html += eventRow(event);
			}
			html += "</table>\n</body></html>\n";
			return html;
		}

		bool WriteFile(const std::filesystem::path& a_path, const std::string& a_content)
		{
			// Write a new file and then put it in the place of the old one,
			// so that a crash in the write does not damage the old file.
			std::filesystem::path temporary = a_path;
			temporary += ".tmp";
			{
				std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
				if (!out) {
					return false;
				}
				out << a_content;
				if (!out) {
					return false;
				}
			}
			std::error_code ec;
			std::filesystem::rename(temporary, a_path, ec);
			return !ec;
		}

		void PutMetrics(std::string& a_out, const char* a_prefix, const Metrics& a_metrics)
		{
			a_out += Fmt("%s.seconds=%d\n%s.averageFps=%.3f\n%s.lowFps=%.3f\n%s.slowPerMinute=%.3f\n%s.averageWorstMs=%.3f\n", a_prefix, a_metrics.seconds,
				a_prefix, static_cast<double>(a_metrics.averageFps), a_prefix, static_cast<double>(a_metrics.lowFps), a_prefix,
				static_cast<double>(a_metrics.slowPerMinute), a_prefix, static_cast<double>(a_metrics.averageWorstMs));
		}
	}

	Metrics ComputeMetrics(const std::vector<Sample>& a_samples, int a_fromSecond, int a_toSecond)
	{
		Metrics            result;
		std::vector<float> fps;
		double             sumFps = 0.0, sumWorst = 0.0;
		int                slow = 0;
		const int          end = std::min(a_toSecond, static_cast<int>(a_samples.size()));
		for (int i = std::max(a_fromSecond, 0); i < end; ++i) {
			const Sample& sample = a_samples[i];
			if (!Counts(sample)) {
				continue;
			}
			fps.push_back(sample.fps);
			sumFps += sample.fps;
			sumWorst += sample.worstFrameMs;
			slow += IsSlow(sample) ? 1 : 0;
		}
		if (fps.empty()) {
			return result;
		}
		result.seconds = static_cast<int>(fps.size());
		result.averageFps = static_cast<float>(sumFps / result.seconds);
		result.averageWorstMs = static_cast<float>(sumWorst / result.seconds);
		result.slowPerMinute = static_cast<float>(slow) * 60.0f / static_cast<float>(result.seconds);
		const std::size_t index = fps.size() * 5 / 100;
		std::nth_element(fps.begin(), fps.begin() + static_cast<std::ptrdiff_t>(index), fps.end());
		result.lowFps = fps[index];
		return result;
	}

	std::string SummaryText(const Summary& a_summary)
	{
		std::string text = "SkyrimDiagnosticsSummary=1\n";
		text += Fmt("durationSeconds=%d\n", a_summary.durationSeconds);
		PutMetrics(text, "first", a_summary.first);
		PutMetrics(text, "total", a_summary.total);
		return text;
	}

	bool ParseSummary(const std::string& a_text, Summary& a_out)
	{
		std::istringstream stream(a_text);
		std::string        line;
		bool               valid = false;
		while (std::getline(stream, line)) {
			const auto equals = line.find('=');
			if (equals == std::string::npos) {
				continue;
			}
			const std::string key = line.substr(0, equals);
			const double      value = std::atof(line.c_str() + equals + 1);
			const auto        metric = [&](const char* a_prefix, Metrics& a_metrics) {
                const std::string prefix = std::string(a_prefix) + ".";
                if (key.rfind(prefix, 0) != 0) {
                    return;
                }
                const std::string field = key.substr(prefix.size());
                if (field == "seconds") {
                    a_metrics.seconds = static_cast<int>(value);
                } else if (field == "averageFps") {
                    a_metrics.averageFps = static_cast<float>(value);
                } else if (field == "lowFps") {
                    a_metrics.lowFps = static_cast<float>(value);
                } else if (field == "slowPerMinute") {
                    a_metrics.slowPerMinute = static_cast<float>(value);
                } else if (field == "averageWorstMs") {
                    a_metrics.averageWorstMs = static_cast<float>(value);
                }
			};
			if (key == "SkyrimDiagnosticsSummary") {
				valid = value == 1.0;
			} else if (key == "durationSeconds") {
				a_out.durationSeconds = static_cast<int>(value);
			} else {
				metric("first", a_out.first);
				metric("total", a_out.total);
			}
		}
		return valid;
	}

	bool Compare(const Metrics& a_current, const std::vector<Summary>& a_previous, std::string& a_text, float& a_change)
	{
		int   count = 0;
		float fps = 0, low = 0, slow = 0;
		for (const auto& previous : a_previous) {
			if (Qualifies(previous) && previous.first.averageFps > 0.0f && previous.first.lowFps > 0.0f) {
				++count;
				fps += previous.first.averageFps;
				low += previous.first.lowFps;
				slow += previous.first.slowPerMinute;
			}
		}
		if (count == 0 || a_current.seconds < kCompareSeconds / 2) {
			return false;
		}
		fps /= count;
		low /= count;
		slow /= count;

		// The change is the average of the change of the average FPS and
		// the change of the low FPS.
		a_change = ((a_current.averageFps / fps - 1.0f) + (a_current.lowFps / low - 1.0f)) / 2.0f;
		const float percent = a_change * 100.0f;
		const char* verdict = percent >= 0.0f ? "better" : "worse";
		a_text = Fmt("This session runs %.0f%% %s than the average of your last %d session%s of 10 minutes or more: average FPS %.0f (before %.0f), "
					 "low FPS %.0f (before %.0f), seconds with a slow frame each minute %.1f (before %.1f). A different place in the game or a different "
					 "frame cap also changes these values.",
			static_cast<double>(percent < 0.0f ? -percent : percent), verdict, count, count == 1 ? "" : "s", static_cast<double>(a_current.averageFps),
			static_cast<double>(fps), static_cast<double>(a_current.lowFps), static_cast<double>(low), static_cast<double>(a_current.slowPerMinute),
			static_cast<double>(slow));
		return true;
	}

	int Prune(const std::filesystem::path& a_dir, int a_keep)
	{
		std::error_code          ec;
		std::vector<std::string> names;
		for (const auto& entry : std::filesystem::directory_iterator(a_dir, ec)) {
			const auto path = entry.path();
			if (path.extension() == ".sum" && path.stem().string().rfind("session-", 0) == 0) {
				names.push_back(path.stem().string());
			}
		}
		// The names have the start time, so the order of the names is the
		// order of the sessions.
		std::sort(names.begin(), names.end());
		const int remove = static_cast<int>(names.size()) - a_keep;
		for (int i = 0; i < remove; ++i) {
			std::filesystem::remove(a_dir / (names[i] + ".sum"), ec);
			std::filesystem::remove(a_dir / (names[i] + ".html"), ec);
		}
		// Reports with no summary (from a write that failed) and old
		// temporary files.
		for (const auto& entry : std::filesystem::directory_iterator(a_dir, ec)) {
			const auto path = entry.path();
			const bool orphan = path.extension() == ".html" && !std::filesystem::exists(a_dir / (path.stem().string() + ".sum"), ec);
			if (orphan || path.extension() == ".tmp") {
				std::filesystem::remove(path, ec);
			}
		}
		return remove > 0 ? a_keep : static_cast<int>(names.size());
	}

	std::string MemoryText(std::uint64_t a_megabytes)
	{
		if (a_megabytes < 1024) {
			return Fmt("%llu MB", static_cast<unsigned long long>(a_megabytes));
		}
		return Fmt("%llu MB (%.1f GB)", static_cast<unsigned long long>(a_megabytes), static_cast<double>(a_megabytes) / 1024.0);
	}

	std::vector<std::pair<std::string, std::vector<std::string>>> ParseSuggestions(const std::string& a_text)
	{
		std::vector<std::pair<std::string, std::vector<std::string>>> result;
		std::istringstream                                            stream(a_text);
		std::string                                                   line;
		while (std::getline(stream, line)) {
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
				line.pop_back();
			}
			std::size_t begin = 0;
			while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t')) {
				++begin;
			}
			line.erase(0, begin);
			if (line.empty() || line[0] == '#') {
				continue;
			}
			if (line.front() == '[' && line.back() == ']') {
				result.push_back({ line.substr(1, line.size() - 2), {} });
			} else if (!result.empty()) {
				result.back().second.push_back(line);
			}
		}
		return result;
	}

	void Begin(const std::filesystem::path& a_dir, const Info& a_info)
	{
		const std::scoped_lock lock{ g_lock };
		g_baseDir = a_dir;
		g_dir = a_dir / "No character";
		g_info = a_info;
		g_group.clear();
		g_problems.clear();
		g_modRows.clear();
		g_scriptRows.clear();
		g_threadRows.clear();
		g_functionRows.clear();
		g_timeRows.clear();
		g_memoryRows.clear();
		g_loadPeriods = 0;

		std::error_code ec;
		std::filesystem::create_directories(g_dir, ec);
		// Session files from an older version of the tool are in a_dir
		// itself. They have no character: move them.
		std::vector<std::filesystem::path> older;
		for (const auto& entry : std::filesystem::directory_iterator(a_dir, ec)) {
			if (entry.is_regular_file(ec) && entry.path().filename().string().rfind("session-", 0) == 0) {
				older.push_back(entry.path());
			}
		}
		for (const auto& path : older) {
			std::filesystem::rename(path, g_dir / path.filename(), ec);
		}
		Prune(g_dir, kKeptSessions);

		const std::tm now = LocalTime(std::time(nullptr));
		g_name = Fmt("session-%04d%02d%02d-%02d%02d%02d", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, now.tm_hour, now.tm_min, now.tm_sec);
		g_startText = Fmt("%04d-%02d-%02d %02d:%02d:%02d", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, now.tm_hour, now.tm_min, now.tm_sec);
		LoadPrevious();
		g_samples.clear();
		g_samples.reserve(4 * 3600);
		g_events.clear();
		g_loads.clear();
		g_compared = false;
		g_started = true;
	}

	bool SetGroup(const char* a_character)
	{
		const std::scoped_lock lock{ g_lock };
		if (!g_started || !g_group.empty() || !a_character || !a_character[0]) {
			return false;
		}
		g_group = a_character;
		const std::filesystem::path previousDir = g_dir;
		g_dir = g_baseDir / SafeName(a_character);

		std::error_code ec;
		std::filesystem::create_directories(g_dir, ec);
		// The files of this session are written again in the new folder.
		std::filesystem::remove(previousDir / (g_name + ".html"), ec);
		std::filesystem::remove(previousDir / (g_name + ".sum"), ec);
		Prune(g_dir, kKeptSessions);
		LoadPrevious();
		g_compared = false;
		return true;
	}

	void NoteProblem(const char* a_id)
	{
		const std::scoped_lock lock{ g_lock };
		if (!a_id || !a_id[0]) {
			return;
		}
		for (const auto& id : g_problems) {
			if (id == a_id) {
				return;
			}
		}
		g_problems.push_back(a_id);
	}

	void SetConfigFile(const std::filesystem::path& a_path)
	{
		const std::scoped_lock lock{ g_lock };
		g_configFile = a_path;
	}

	void AddModLoad(const char* a_name, float a_ownCores, float a_waitingCores, float a_totalCores)
	{
		const std::scoped_lock lock{ g_lock };
		LoadRow& row = Row(g_modRows, a_name, "");
		row.sumA += a_ownCores;
		row.sumB += a_waitingCores;
		row.sumC += a_totalCores;
		row.peakA = std::max(row.peakA, a_ownCores);
	}

	void AddScriptLoad(const char* a_name, float a_running, float a_waiting)
	{
		const std::scoped_lock lock{ g_lock };
		LoadRow& row = Row(g_scriptRows, a_name, "");
		row.sumA += a_running;
		row.sumB += a_waiting;
		row.peakA = std::max(row.peakA, a_running);
	}

	void AddThreadWork(const char* a_role, const char* a_module, float a_cores)
	{
		const std::scoped_lock lock{ g_lock };
		LoadRow& row = Row(g_threadRows, a_module, a_role);
		row.sumA += a_cores;
		row.peakA = std::max(row.peakA, a_cores);
	}

	void AddFunctionLoad(const char* a_name, float a_running)
	{
		const std::scoped_lock lock{ g_lock };
		LoadRow& row = Row(g_functionRows, a_name, "");
		row.sumA += a_running;
		row.peakA = std::max(row.peakA, a_running);
	}

	void AddFunctionTime(const char* a_name, float a_msPerSecond, float a_callsPerSecond)
	{
		const std::scoped_lock lock{ g_lock };
		LoadRow& row = Row(g_timeRows, a_name, "");
		row.sumA += a_msPerSecond;
		row.sumB += a_callsPerSecond;
		row.peakA = std::max(row.peakA, a_msPerSecond);
	}

	void SetModMemory(const char* a_name, float a_inUseMb, float a_allocatedMb)
	{
		const std::scoped_lock lock{ g_lock };
		LoadRow& row = Row(g_memoryRows, a_name, "");
		row.sumA = a_inUseMb;
		row.sumB = a_allocatedMb;
		row.peakA = std::max(row.peakA, a_inUseMb);
	}

	void EndLoadPeriod()
	{
		const std::scoped_lock lock{ g_lock };
		++g_loadPeriods;
	}

	void AddSample(const Sample& a_sample)
	{
		const std::scoped_lock lock{ g_lock };
		if (g_started && g_samples.size() < 48 * 3600) {
			g_samples.push_back(a_sample);
		}
	}

	void AddEvent(const char* a_text)
	{
		const std::scoped_lock lock{ g_lock };
		if (g_started && a_text && g_events.size() < 20000) {
			g_events.push_back({ static_cast<int>(g_samples.size()), a_text, g_cell });

			// "Loading screen: 2.7 s, ..." comes at the end of the load.
			constexpr char prefix[] = "Loading screen: ";
			if (std::string_view(a_text).rfind(prefix, 0) == 0) {
				const float end = static_cast<float>(g_samples.size());
				const float seconds = static_cast<float>(std::atof(a_text + sizeof(prefix) - 1));
				g_loads.push_back({ std::max(end - std::max(seconds, 0.0f), 0.0f), end });
			}
		}
	}

	void SetCell(const char* a_cell)
	{
		const std::scoped_lock lock{ g_lock };
		g_cell = a_cell ? a_cell : "";
	}

	bool Write()
	{
		const std::scoped_lock lock{ g_lock };
		if (!g_started) {
			return false;
		}
		const bool html = WriteFile(g_dir / (g_name + ".html"), BuildHtml());
		const bool sum = WriteFile(g_dir / (g_name + ".sum"), SummaryText(CurrentSummary()));
		return html && sum;
	}

	bool CheckComparison(std::string& a_text, float& a_change)
	{
		const std::scoped_lock lock{ g_lock };
		if (!g_started || g_compared || static_cast<int>(g_samples.size()) < kCompareSeconds) {
			return false;
		}
		g_compared = true;
		return Compare(ComputeMetrics(g_samples, kSkipSeconds, kCompareSeconds), g_previous, a_text, a_change);
	}
}
