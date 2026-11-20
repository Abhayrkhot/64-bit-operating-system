// printf-style formatting shared by the kernel, user programs and host tools.
// No floating point: the kernel is built without SSE/x87.
#pragma once

#include <stdarg.h>
#include <stddef.h>

namespace os::fmt {

// Supports flags "-0 +#", width, precision (strings and integers), length
// modifiers hh h l ll z j t and conversions d i u x X o p s c %.
// Returns the number of characters the full output needs (like vsnprintf).
int vformat(char* buf, size_t size, const char* fmt, va_list ap);
int format(char* buf, size_t size, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

}  // namespace os::fmt
