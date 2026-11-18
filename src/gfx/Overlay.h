#pragma once

// Present hook, frame measurement and the in-game overlay.
// Direct3D 11 only: no CommonLibSSE, no Dear ImGui. AbyssusLib draws the
// overlay windows (Abyssus/API.h).

namespace skydiag::overlay
{
	struct Options
	{
		int   view{ 0 };                  // 0 = all, 1 = warnings only, 2 = off
		int   toggleKey{ 0x79 };          // Windows virtual key code
		float warningHoldSeconds{ 30.0f };
		float fpsBelow{ 57.0f };
		bool  profiler{ true };
		bool  memoryByDll{ true };
		// The [[Layout]] part of the configuration file, or nullptr.
		const char* layoutText{ nullptr };
	};

	// Hooks the Present function of the swap chain. a_swapChain is an
	// IDXGISwapChain pointer. a_version is shown in the overlay title.
	// Call one time. Returns false on failure.
	bool Install(void* a_swapChain, const char* a_version, const Options& a_options);

	// Shows a text in the note box of AbyssusLib for a_seconds, if the view
	// is not "off". Safe from all threads. A new note replaces the note
	// that shows.
	void PostNote(const char* a_text, float a_seconds);

	// Values that the plugin reads from the game. 0 = not known.
	struct EngineState
	{
		unsigned references{ 0 };       // object references in use
		unsigned referenceLimit{ 0 };
		unsigned fullPlugins{ 0 };      // all full plugins
		unsigned lightPlugins{ 0 };     // all light plugins
		unsigned gameFullPlugins{ 0 };  // of these: the game, its DLCs and Creation Club content
		unsigned gameLightPlugins{ 0 };
		unsigned animations{ 0 };       // loaded animation files
		bool     animationsKnown{ false };  // false: the count could not be read
		// True from the game start until 1 minute after the first loading
		// screen. The plugin warning shows only in this time.
		bool     pluginWarningTime{ true };
	};
	void SetEngineState(const EngineState& a_state);

	// Tells the overlay that the game reads the input of a frame now. The
	// input latency is measured from this time. Safe from all threads.
	void NoteInputRead();

	// Draw calls in each frame, average of the last half second. Safe
	// from all threads.
	unsigned GetDrawCallsPerFrame();

	// Video memory in megabytes, 0 if not known. Safe from all threads.
	void GetVideoMemoryMb(unsigned& a_used, unsigned& a_budget);
}
