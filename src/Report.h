#pragma once

// Session history.
//
// Keeps one sample for each second of the session and all events, and writes
// two files for the session:
//   session-<time>.html   a report with graphs, warnings, loads and events
//   session-<time>.sum    a short summary that later sessions compare with
// The files are written again one time each minute, so a crash loses at
// most one minute. Only the newest sessions are kept.
//
// Standard C++ only: testable without the game.

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace skydiag::report
{
	constexpr int   kKeptSessions = 10;        // older sessions are deleted at game start
	constexpr int   kCompareSeconds = 600;     // the comparison uses the first 10 minutes
	constexpr int   kSkipSeconds = 30;         // the game start is not part of the comparison
	constexpr float kNotifyChange = 0.10f;     // 10 %

	struct Sample
	{
		float         fps{ 0 };
		float         worstFrameMs{ 0 };
		std::uint32_t gameMb{ 0 };
		std::uint32_t gameMaxMb{ 0 };    // the most that the game can have: used plus what the system can still give
		std::uint32_t vramMb{ 0 };
		std::uint32_t vramBudgetMb{ 0 }; // the video memory that the system gives to the game
		float         papyrusRunning{ 0 };
		std::uint32_t drawCalls{ 0 };        // draw calls in each frame, average
		std::uint32_t references{ 0 };       // object references in use, 0 = not known
		std::uint32_t referenceLimit{ 0 };
		std::uint32_t animations{ 0 };       // loaded animation files, 0 = not known
		std::uint32_t responseMs{ 0 };
		bool          loading{ false };  // a loading screen is open
		bool          active{ true };    // the game window is the active window
	};

	struct Info
	{
		std::string version;
		std::string game;
		std::string platform;
	};

	// Performance values of a period. Only seconds in which the game ran
	// normally count: no loading screen, window active, frames shown.
	struct Metrics
	{
		int   seconds{ 0 };          // seconds that count
		float averageFps{ 0 };
		float lowFps{ 0 };           // 95 % of the seconds have a higher FPS
		float slowPerMinute{ 0 };    // seconds with a slow frame, for each minute
		float averageWorstMs{ 0 };   // average of the slowest frame of each second
	};

	struct Summary
	{
		std::string name;            // file name without extension
		int         durationSeconds{ 0 };
		Metrics     first;           // the first 10 minutes
		Metrics     total;
	};

	// Starts the history in the folder a_dir/"No character". Deletes old
	// session files there so that kKeptSessions stay, and reads their
	// summaries.
	void Begin(const std::filesystem::path& a_dir, const Info& a_info);

	// The character of the save that was loaded. The session files move to
	// the folder a_dir/<character>, and the comparison uses only the
	// earlier sessions of this character: a different character can have a
	// different mod list. Only the first call in a session has an effect.
	// Returns true if this call set the character.
	bool SetGroup(const char* a_character);

	// A problem type that occurred (the id of a warning, for example
	// "fps-cpu"). The report lists the suggestions for it.
	void NoteProblem(const char* a_id);

	// The configuration file. The report reads its [[Suggestions]] part
	// each time it is written, so the user can change the suggestions
	// while the game runs.
	void SetConfigFile(const std::filesystem::path& a_path);

	// Processor load data for the report tables. Call these for one period
	// (about 5 seconds), then EndLoadPeriod.
	void AddModLoad(const char* a_name, float a_ownCores, float a_waitingCores, float a_totalCores);
	void AddScriptLoad(const char* a_name, float a_running, float a_waiting);
	void AddThreadWork(const char* a_role, const char* a_module, float a_cores);
	void AddFunctionLoad(const char* a_name, float a_running);  // Papyrus function "Script.Function"
	void AddFunctionTime(const char* a_name, float a_msPerSecond, float a_callsPerSecond);  // measured
	void EndLoadPeriod();

	// Heap memory of a mod DLL now, in megabytes. The report keeps the
	// last value and the largest value.
	void SetModMemory(const char* a_name, float a_inUseMb, float a_allocatedMb);

	void AddSample(const Sample& a_sample);  // one time each second
	void AddEvent(const char* a_text);       // safe from all threads

	// The place of the player (cell name), stored with each event from now on.
	void SetCell(const char* a_cell);

	// Writes the report and the summary. Returns false on a file error.
	bool Write();

	// The cause of the last failed Write(), or an empty text.
	std::string LastWriteError();

	// One time, when the session is 10 minutes old: compares its first 10
	// minutes with the average of the earlier sessions that were 10 minutes
	// or longer. Returns true if there is a result. a_change is the change
	// of the frame rate values (0.12 = 12 % better, -0.12 = 12 % worse).
	bool CheckComparison(std::string& a_text, float& a_change);

	// Parts that the tests use.
	Metrics     ComputeMetrics(const std::vector<Sample>& a_samples, int a_fromSecond, int a_toSecond);
	std::string SummaryText(const Summary& a_summary);
	bool        ParseSummary(const std::string& a_text, Summary& a_out);
	bool        Compare(const Metrics& a_current, const std::vector<Summary>& a_previous, std::string& a_text, float& a_change);
	int         Prune(const std::filesystem::path& a_dir, int a_keep);  // returns the number of sessions that stay
	std::string MemoryText(std::uint64_t a_megabytes);                  // "1451 MB (1.4 GB)" from 1024 MB
	// Reads the suggestions text: for each problem id, the lines of its section.
	std::vector<std::pair<std::string, std::vector<std::string>>> ParseSuggestions(const std::string& a_text);
}
