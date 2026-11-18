#pragma once

// Text for a memory size. From 1 GB the text has megabytes and gigabytes:
// "1451 MB (1.4 GB)". No heap memory: safe after a crash.

#include <cstdint>
#include <cstdio>

namespace skydiag
{
	struct MemText
	{
		char text[48];

		explicit MemText(std::uint64_t a_megabytes)
		{
			if (a_megabytes < 1024) {
				std::snprintf(text, sizeof(text), "%llu MB", static_cast<unsigned long long>(a_megabytes));
			} else {
				std::snprintf(text, sizeof(text), "%llu MB (%.1f GB)", static_cast<unsigned long long>(a_megabytes),
					static_cast<double>(a_megabytes) / 1024.0);
			}
		}
	};
}
