#include "fmt/format.hpp"

#include <stdint.h>

namespace shale::fmt {
namespace {

struct Out {
  char* buf;
  size_t size;
  size_t pos = 0;
  void put(char c) {
    if (pos + 1 < size) buf[pos] = c;
    pos++;
  }
  void pad(char c, int n) {
    while (n-- > 0) put(c);
  }
};

struct Spec {
  bool left = false, zero = false, plus = false, space = false, alt = false;
  int width = 0;
  int precision = -1;
};

void emit_number(Out& out, const Spec& spec, uint64_t v, bool negative, unsigned base, bool upper,
                 const char* prefix) {
  char digits[24];
  int n = 0;
  const char* set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  if (v == 0 && spec.precision != 0) digits[n++] = '0';
  while (v) {
    digits[n++] = set[v % base];
    v /= base;
  }
  int zeros = spec.precision > n ? spec.precision - n : 0;
  char sign = negative ? '-' : spec.plus ? '+' : spec.space ? ' ' : 0;
  int prefix_len = 0;
  while (prefix && prefix[prefix_len]) prefix_len++;
  int body = n + zeros + (sign ? 1 : 0) + prefix_len;
  int fill = spec.width > body ? spec.width - body : 0;
  bool zero_fill = spec.zero && !spec.left && spec.precision < 0;
  if (!spec.left && !zero_fill) out.pad(' ', fill);
  if (sign) out.put(sign);
  for (int i = 0; i < prefix_len; i++) out.put(prefix[i]);
  if (zero_fill) out.pad('0', fill);
  out.pad('0', zeros);
  while (n) out.put(digits[--n]);
  if (spec.left) out.pad(' ', fill);
}

}  // namespace

int vformat(char* buf, size_t size, const char* fmt, va_list ap) {
  Out out{buf, size};
  for (const char* p = fmt; *p; p++) {
    if (*p != '%') {
      out.put(*p);
      continue;
    }
    p++;
    Spec spec;
    for (;; p++) {
      if (*p == '-') spec.left = true;
      else if (*p == '0') spec.zero = true;
      else if (*p == '+') spec.plus = true;
      else if (*p == ' ') spec.space = true;
      else if (*p == '#') spec.alt = true;
      else break;
    }
    if (*p == '*') {
      spec.width = va_arg(ap, int);
      if (spec.width < 0) {
        spec.left = true;
        spec.width = -spec.width;
      }
      p++;
    } else {
      while (*p >= '0' && *p <= '9') spec.width = spec.width * 10 + (*p++ - '0');
    }
    if (*p == '.') {
      p++;
      spec.precision = 0;
      if (*p == '*') {
        spec.precision = va_arg(ap, int);
        p++;
      } else {
        while (*p >= '0' && *p <= '9') spec.precision = spec.precision * 10 + (*p++ - '0');
      }
    }
    int length = 0;  // 0 int, 1 long, 2 long long, -1 short, -2 char, 3 size_t/intmax/ptrdiff
    if (*p == 'h') {
      length = -1;
      if (*++p == 'h') {
        length = -2;
        p++;
      }
    } else if (*p == 'l') {
      length = 1;
      if (*++p == 'l') {
        length = 2;
        p++;
      }
    } else if (*p == 'z' || *p == 'j' || *p == 't') {
      length = 3;
      p++;
    }

    auto get_unsigned = [&]() -> uint64_t {
      switch (length) {
        case -2: return static_cast<unsigned char>(va_arg(ap, unsigned int));
        case -1: return static_cast<unsigned short>(va_arg(ap, unsigned int));
        case 0: return va_arg(ap, unsigned int);
        case 1: return va_arg(ap, unsigned long);
        case 2: return va_arg(ap, unsigned long long);
        default: return va_arg(ap, size_t);
      }
    };
    auto get_signed = [&]() -> int64_t {
      switch (length) {
        case -2: return static_cast<signed char>(va_arg(ap, int));
        case -1: return static_cast<short>(va_arg(ap, int));
        case 0: return va_arg(ap, int);
        case 1: return va_arg(ap, long);
        case 2: return va_arg(ap, long long);
        default: return va_arg(ap, long);
      }
    };

    switch (*p) {
      case 'd':
      case 'i': {
        int64_t v = get_signed();
        uint64_t mag = v < 0 ? 0 - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
        emit_number(out, spec, mag, v < 0, 10, false, nullptr);
        break;
      }
      case 'u': emit_number(out, spec, get_unsigned(), false, 10, false, nullptr); break;
      case 'x': {
        uint64_t v = get_unsigned();
        emit_number(out, spec, v, false, 16, false, spec.alt && v ? "0x" : nullptr);
        break;
      }
      case 'X': {
        uint64_t v = get_unsigned();
        emit_number(out, spec, v, false, 16, true, spec.alt && v ? "0X" : nullptr);
        break;
      }
      case 'o': {
        uint64_t v = get_unsigned();
        emit_number(out, spec, v, false, 8, false, spec.alt && v ? "0" : nullptr);
        break;
      }
      case 'p': {
        auto v = reinterpret_cast<uintptr_t>(va_arg(ap, void*));
        Spec ps = spec;
        if (ps.precision < 0) ps.precision = 16;
        emit_number(out, ps, v, false, 16, false, "0x");
        break;
      }
      case 'c': {
        char c = static_cast<char>(va_arg(ap, int));
        if (!spec.left) out.pad(' ', spec.width - 1);
        out.put(c);
        if (spec.left) out.pad(' ', spec.width - 1);
        break;
      }
      case 's': {
        const char* s = va_arg(ap, const char*);
        if (!s) s = "(null)";
        int n = 0;
        while (s[n] && (spec.precision < 0 || n < spec.precision)) n++;
        if (!spec.left) out.pad(' ', spec.width - n);
        for (int i = 0; i < n; i++) out.put(s[i]);
        if (spec.left) out.pad(' ', spec.width - n);
        break;
      }
      case '%': out.put('%'); break;
      case 0: p--; break;  // trailing '%'
      default:
        out.put('%');
        out.put(*p);
        break;
    }
  }
  if (size) buf[out.pos < size ? out.pos : size - 1] = 0;
  return static_cast<int>(out.pos);
}

int format(char* buf, size_t size, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vformat(buf, size, fmt, ap);
  va_end(ap);
  return n;
}

}  // namespace shale::fmt
