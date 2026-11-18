#pragma once

// The layout of the overlay: which parts show, and in which order. The
// text comes from the [[Layout]] part of the configuration file.
// Standard C++ only.

#include <string>
#include <vector>

namespace skydiag::layout
{
	enum class Part
	{
		kSeparator,  // a line between sections
		kFps,
		kGraph,
		kPresent,
		kLimit,
		kDrawCalls,
		kLatency,
		kResponse,
		kMemory,
		kVram,
		kReferences,
		kPlugins,
		kAnimations,
		kModMemory,
		kThreads,
		kModDlls,
		kPapyrus,
		kScripts,
		kFunctions,
	};

	// Reads a layout text. Lines that are not known are ignored. Each
	// section that shows starts with a separator, but not the first one.
	// A text with no parts gives an empty list.
	std::vector<Part> Parse(const std::string& a_text);
}
