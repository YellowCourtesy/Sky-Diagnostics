#pragma once

// Text output into a fixed buffer. No heap memory: safe after a crash.

#include <cstdarg>
#include <cstddef>
#include <cstdio>

namespace skydiag
{
	struct Out
	{
		char*       buf;
		std::size_t cap;
		std::size_t len;
	};

	inline void Put(Out& a_out, const char* a_fmt, ...)
	{
		if (a_out.len + 1 >= a_out.cap) {
			return;
		}
		va_list args;
		va_start(args, a_fmt);
		const int n = _vsnprintf_s(a_out.buf + a_out.len, a_out.cap - a_out.len, _TRUNCATE, a_fmt, args);
		va_end(args);
		if (n < 0) {
			a_out.len = a_out.cap - 1;  // truncated
		} else {
			a_out.len += static_cast<std::size_t>(n);
		}
	}
}
