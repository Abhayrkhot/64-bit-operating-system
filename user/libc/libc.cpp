#include "libc.hpp"

#include "fmt/format.hpp"

extern "C" void (*__init_array_start[])(), (*__init_array_end[])();

extern "C" [[noreturn]] void __libc_start(int argc, char** argv) {
  for (auto* fn = __init_array_start; fn < __init_array_end; fn++) (*fn)();
  exit(main(argc, argv));
}

void exit(int code) {
  syscall6(sys::kExit, static_cast<uint64_t>(code));
  __builtin_unreachable();
}

// ---- stdio -----------------------------------------------------------------

namespace {
int vdprintf(int fd, const char* fmt, va_list ap) {
  char buf[1024];
  int n = os::fmt::vformat(buf, sizeof(buf), fmt, ap);
  write(fd, buf, static_cast<uint64_t>(n < static_cast<int>(sizeof(buf)) ? n : sizeof(buf) - 1));
  return n;
}
}  // namespace

int printf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vdprintf(1, fmt, ap);
  va_end(ap);
  return n;
}

int dprintf(int fd, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vdprintf(fd, fmt, ap);
  va_end(ap);
  return n;
}

int snprintf(char* buf, size_t size, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = os::fmt::vformat(buf, size, fmt, ap);
  va_end(ap);
  return n;
}

int puts(const char* s) {
  write(1, s, strlen(s));
  write(1, "\n", 1);
  return 0;
}

int64_t read_line(int fd, char* buf, uint64_t size) {
  uint64_t n = 0;
  while (n + 1 < size) {
    char c;
    int64_t r = read(fd, &c, 1);
    if (r <= 0) {
      if (n == 0) return -1;
      break;
    }
    if (c == '\n') break;
    buf[n++] = c;
  }
  buf[n] = 0;
  return static_cast<int64_t>(n);
}

// ---- malloc: size classes over brk, large blocks via mmap -------------------
//
// Each block has a 16-byte header {size class or mapped length, magic}.
// Blocks up to 128 KiB come from power-of-two free lists carved from the brk
// heap; larger ones are their own mmap region.

namespace {
constexpr uint64_t kMagic = 0x6d616c6c6f63u;  // "malloc"
constexpr uint64_t kMappedFlag = 1ull << 63;
constexpr unsigned kClasses = 14;  // 16, 32, ..., 128 KiB payload bytes
struct Header {
  uint64_t size;  // payload capacity, or mapped length | kMappedFlag
  uint64_t magic;
};
struct FreeNode {
  FreeNode* next;
};
FreeNode* g_free[kClasses];
uint64_t g_brk_cur, g_brk_end;

unsigned class_of(size_t n) {
  unsigned c = 0;
  while ((16ull << c) < n) c++;
  return c;
}

void* carve(uint64_t bytes) {
  if (!g_brk_cur) g_brk_cur = g_brk_end = brk(0);
  if (g_brk_cur + bytes > g_brk_end) {
    uint64_t want = os::align_up(g_brk_cur + bytes, 64 * 1024);
    if (brk(want) != want) return nullptr;
    g_brk_end = want;
  }
  void* p = reinterpret_cast<void*>(g_brk_cur);
  g_brk_cur += bytes;
  return p;
}
}  // namespace

void* malloc(size_t n) {
  if (n == 0) n = 1;
  if (n <= (16u << (kClasses - 1))) {
    unsigned c = class_of(n);
    Header* h;
    if (FreeNode* node = g_free[c]) {
      g_free[c] = node->next;
      h = reinterpret_cast<Header*>(node);
    } else {
      h = static_cast<Header*>(carve(sizeof(Header) + (16ull << c)));
      if (!h) return nullptr;
    }
    h->size = 16ull << c;
    h->magic = kMagic;
    return h + 1;
  }
  uint64_t len = os::align_up(n + sizeof(Header), 4096);
  int64_t addr = mmap(len, sys::kProtRead | sys::kProtWrite);
  if (addr < 0) return nullptr;
  auto* h = reinterpret_cast<Header*>(addr);
  h->size = len | kMappedFlag;
  h->magic = kMagic;
  return h + 1;
}

void free(void* p) {
  if (!p) return;
  Header* h = static_cast<Header*>(p) - 1;
  if (h->magic != kMagic) {
    dprintf(2, "free: bad pointer %p\n", p);
    exit(134);
  }
  h->magic = 0;
  if (h->size & kMappedFlag) {
    munmap(reinterpret_cast<uint64_t>(h), h->size & ~kMappedFlag);
    return;
  }
  unsigned c = class_of(h->size);
  auto* node = reinterpret_cast<FreeNode*>(h);
  node->next = g_free[c];
  g_free[c] = node;
}

void* calloc(size_t count, size_t size) {
  size_t total;
  if (__builtin_mul_overflow(count, size, &total)) return nullptr;
  void* p = malloc(total);
  if (p) memset(p, 0, total);
  return p;
}

void* realloc(void* p, size_t n) {
  if (!p) return malloc(n);
  Header* h = static_cast<Header*>(p) - 1;
  uint64_t cap = (h->size & kMappedFlag) ? (h->size & ~kMappedFlag) - sizeof(Header) : h->size;
  if (n <= cap) return p;
  void* q = malloc(n);
  if (!q) return nullptr;
  memcpy(q, p, cap);
  free(p);
  return q;
}

void* operator new(size_t n) { return malloc(n); }
void* operator new[](size_t n) { return malloc(n); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

// ---- strings -----------------------------------------------------------------

char* strcpy(char* dst, const char* src) {
  char* d = dst;
  while ((*d++ = *src++)) {
  }
  return dst;
}

char* strchr(const char* s, int c) {
  for (; *s; s++)
    if (*s == static_cast<char>(c)) return const_cast<char*>(s);
  return c == 0 ? const_cast<char*>(s) : nullptr;
}

long atol(const char* s) {
  long v = 0;
  bool neg = *s == '-';
  if (neg) s++;
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return neg ? -v : v;
}

const char* errno_name(int64_t err) {
  switch (-err) {
    case sys::kEPERM: return "EPERM";
    case sys::kENOENT: return "ENOENT";
    case sys::kESRCH: return "ESRCH";
    case sys::kEIO: return "EIO";
    case sys::kE2BIG: return "E2BIG";
    case sys::kENOEXEC: return "ENOEXEC";
    case sys::kEBADF: return "EBADF";
    case sys::kECHILD: return "ECHILD";
    case sys::kENOMEM: return "ENOMEM";
    case sys::kEFAULT: return "EFAULT";
    case sys::kEEXIST: return "EEXIST";
    case sys::kENOTDIR: return "ENOTDIR";
    case sys::kEISDIR: return "EISDIR";
    case sys::kEINVAL: return "EINVAL";
    case sys::kEMFILE: return "EMFILE";
    case sys::kEFBIG: return "EFBIG";
    case sys::kENOSPC: return "ENOSPC";
    case sys::kESPIPE: return "ESPIPE";
    case sys::kEROFS: return "EROFS";
    case sys::kENAMETOOLONG: return "ENAMETOOLONG";
    case sys::kENOSYS: return "ENOSYS";
    case sys::kENOTEMPTY: return "ENOTEMPTY";
    default: return err >= 0 ? "ok" : "E?";
  }
}
