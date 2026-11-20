#include "fs/initrd.hpp"

#include "common/mem.hpp"
#include "core/boot.hpp"
#include "core/log.hpp"

namespace initrd {
namespace {

struct Entry {
  char name[100];
  const uint8_t* data;
  uint64_t size;
};
constexpr unsigned kMaxEntries = 128;
Entry g_entries[kMaxEntries];
unsigned g_count;

uint64_t octal(const char* p, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + static_cast<uint64_t>(p[i] - '0');
  return v;
}

// Strips leading "./" and "/".
const char* normalize(const char* p) {
  while (*p == '.' && p[1] == '/') p += 2;
  while (*p == '/') p++;
  return p;
}

}  // namespace

void init(uint64_t phys, uint64_t size) {
  if (!phys) {
    kprintf("initrd: none\n");
    return;
  }
  const uint8_t* base = phys_to_virt<const uint8_t>(phys);
  for (uint64_t off = 0; off + 512 <= size && g_count < kMaxEntries;) {
    const char* h = reinterpret_cast<const char*>(base + off);
    if (h[0] == 0) break;  // end-of-archive marker
    if (memcmp(h + 257, "ustar", 5) != 0) panic("initrd: bad tar header at %lu", off);
    uint64_t fsize = octal(h + 124, 12);
    char type = h[156];
    if ((type == '0' || type == 0) && off + 512 + fsize <= size) {
      Entry& e = g_entries[g_count++];
      const char* n = normalize(h);
      size_t len = strlen(n) < sizeof(e.name) - 1 ? strlen(n) : sizeof(e.name) - 1;
      memcpy(e.name, n, len);
      e.name[len] = 0;
      e.data = base + off + 512;
      e.size = fsize;
    }
    off += 512 + os::align_up(fsize, 512);
  }
  kprintf("initrd: %u files\n", g_count);
}

bool find(const char* path, const uint8_t** data, uint64_t* size) {
  const char* p = normalize(path);
  for (unsigned i = 0; i < g_count; i++) {
    if (strcmp(g_entries[i].name, p) == 0) {
      *data = g_entries[i].data;
      *size = g_entries[i].size;
      return true;
    }
  }
  return false;
}

bool entry(uint64_t index, const char** name, uint64_t* size) {
  if (index >= g_count) return false;
  *name = g_entries[index].name;
  *size = g_entries[index].size;
  return true;
}

}  // namespace initrd
