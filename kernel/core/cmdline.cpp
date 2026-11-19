#include "core/cmdline.hpp"

#include "common/mem.hpp"

namespace cmdline {
namespace {
char g_text[256];
}

void init(const char* text) {
  size_t n = strlen(text);
  if (n >= sizeof(g_text)) n = sizeof(g_text) - 1;
  memcpy(g_text, text, n);
  g_text[n] = 0;
  // fw_cfg strings may carry a trailing newline.
  while (n && (g_text[n - 1] == '\n' || g_text[n - 1] == '\r' || g_text[n - 1] == ' ')) g_text[--n] = 0;
}

const char* raw() { return g_text; }

bool get(const char* key, char* out, size_t size) {
  size_t klen = strlen(key);
  const char* p = g_text;
  while (*p) {
    while (*p == ' ') p++;
    const char* word = p;
    while (*p && *p != ' ') p++;
    size_t wlen = static_cast<size_t>(p - word);
    if (wlen >= klen && strncmp(word, key, klen) == 0 && (wlen == klen || word[klen] == '=')) {
      const char* v = wlen == klen ? word + klen : word + klen + 1;
      size_t vlen = static_cast<size_t>(p - v);
      if (size) {
        size_t n = vlen < size - 1 ? vlen : size - 1;
        memcpy(out, v, n);
        out[n] = 0;
      }
      return true;
    }
  }
  return false;
}

bool has(const char* key) { return get(key, nullptr, 0); }

long get_long(const char* key, long fallback) {
  char buf[32];
  if (!get(key, buf, sizeof(buf)) || !buf[0]) return fallback;
  long v = 0;
  for (const char* p = buf; *p; p++) {
    if (*p < '0' || *p > '9') return fallback;
    v = v * 10 + (*p - '0');
  }
  return v;
}

}  // namespace cmdline
