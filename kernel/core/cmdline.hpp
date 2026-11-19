// Kernel command line: space-separated key=value words, e.g.
//   "test=all"  "bench=ctxswitch iters=200000"  "init=/bin/sh"
#pragma once

#include <stddef.h>

namespace cmdline {

void init(const char* text);
const char* raw();
// Copies the value of `key` into out. A bare word ("quiet") has value "".
bool get(const char* key, char* out, size_t size);
bool has(const char* key);
long get_long(const char* key, long fallback);

}  // namespace cmdline
