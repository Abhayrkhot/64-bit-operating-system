// File-system stress workload for the QEMU kill test (tools/crashtest.py).
//
//   fsstress <seed> <ops>
//
// Prints a durability log on the console. A line is printed by one write()
// before the syscall returns, so if the host saw it before killing QEMU, it
// happened:
//   MODIFY <path>                  about to change <path>
//   DURABLE <path> <len> <crc>     fsync/sync returned: this content is durable
//   REPLACE-BEGIN <path> <len> <crc>  about to atomically replace <path>
//   REPLACE <path> <len> <crc>     the replace and an fsync of <path> returned
// After the kill, the host recovers the disk and checks every DURABLE file
// not MODIFYed since, and that replaced files hold a complete version.
#include "libc.hpp"
#include "util/crc32c.hpp"
#include "util/random.hpp"

namespace {

constexpr int kFiles = 12;
constexpr uint64_t kMaxFile = 256 * 1024;

struct Model {
  bool exists;
  uint8_t* data;
  uint64_t len;
};
Model g_files[kFiles];

void path_of(int i, char* out) { snprintf(out, 32, "/stress/f%02d", i); }

uint32_t crc_of(const Model& m) { return os::crc32c(m.data, m.len); }

void report_durable(int i) {
  char p[32];
  path_of(i, p);
  printf("DURABLE %s %lu %u\n", p, g_files[i].len, crc_of(g_files[i]));
}

void fill(os::Random& rng, uint8_t* p, uint64_t n) {
  for (uint64_t i = 0; i < n; i++) p[i] = static_cast<uint8_t>(rng.next());
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t seed = argc > 1 ? static_cast<uint64_t>(atol(argv[1])) : 1;
  long ops = argc > 2 ? atol(argv[2]) : 2000;
  os::Random rng(seed);
  mkdir("/stress");
  for (auto& f : g_files) f.data = static_cast<uint8_t*>(malloc(kMaxFile));
  auto* scratch = static_cast<uint8_t*>(malloc(kMaxFile));
  printf("FSSTRESS START seed=%lu ops=%ld\n", seed, ops);
  for (long op = 0; op < ops; op++) {
    int i = static_cast<int>(rng.below(kFiles));
    Model& m = g_files[i];
    char p[32];
    path_of(i, p);
    uint64_t kind = rng.below(100);
    if (kind < 40) {  // write somewhere in the file
      uint64_t off = m.exists && m.len && rng.chance(1, 2) ? rng.below(m.len) : (rng.chance(1, 3) ? m.len : 0);
      uint64_t n = rng.chance(1, 8) ? 20000 + rng.below(100000) : 1 + rng.below(6000);
      if (off + n > kMaxFile) n = kMaxFile - off;
      if (n == 0) continue;
      fill(rng, scratch, n);
      printf("MODIFY %s\n", p);
      int fd = open(p, sys::kOWrite | sys::kOCreate);
      if (fd < 0) continue;
      lseek(fd, static_cast<int64_t>(off), sys::kSeekSet);
      if (write(fd, scratch, n) == static_cast<int64_t>(n)) {
        if (!m.exists) m.len = 0;
        if (off > m.len) memset(m.data + m.len, 0, off - m.len);
        memcpy(m.data + off, scratch, n);
        if (off + n > m.len) m.len = off + n;
        m.exists = true;
      }
      close(fd);
    } else if (kind < 60) {  // fsync
      if (!m.exists) continue;
      int fd = open(p, 0);
      if (fd >= 0 && fsync(fd) == 0) report_durable(i);
      close(fd);
    } else if (kind < 68) {  // truncate
      if (!m.exists) continue;
      uint64_t len = rng.below(m.len + 1);
      printf("MODIFY %s\n", p);
      if (truncate(p, len) == 0) m.len = len;
    } else if (kind < 75) {  // unlink
      if (!m.exists) continue;
      printf("MODIFY %s\n", p);
      if (unlink(p) == 0) m.exists = false;
    } else if (kind < 80) {  // sync: everything is durable
      if (sync() == 0)
        for (int k = 0; k < kFiles; k++)
          if (g_files[k].exists) report_durable(k);
    } else if (kind < 92) {  // atomic replace of /stress/cfgN
      char target[32], tmp[40];
      snprintf(target, sizeof(target), "/stress/cfg%d", static_cast<int>(rng.below(2)));
      snprintf(tmp, sizeof(tmp), "%s.tmp", target);
      uint64_t n = 64 + rng.below(30000);
      fill(rng, scratch, n);
      uint32_t crc = os::crc32c(scratch, n);
      int fd = open(tmp, sys::kOWrite | sys::kOCreate | sys::kOTruncate);
      if (fd < 0) continue;
      bool ok = write(fd, scratch, n) == static_cast<int64_t>(n) && fsync(fd) == 0;
      close(fd);
      if (!ok) continue;
      printf("REPLACE-BEGIN %s %lu %u\n", target, n, crc);
      if (rename(tmp, target) != 0) continue;
      fd = open(target, 0);
      if (fd >= 0 && fsync(fd) == 0) printf("REPLACE %s %lu %u\n", target, n, crc);
      close(fd);
    } else {  // read back and compare with the model
      if (!m.exists) continue;
      int fd = open(p, 0);
      int64_t r = read(fd, scratch, kMaxFile);
      close(fd);
      if (r != static_cast<int64_t>(m.len) || memcmp(scratch, m.data, m.len) != 0) {
        printf("FSSTRESS MISMATCH %s\n", p);
        return 1;
      }
    }
  }
  printf("FSSTRESS DONE\n");
  return 0;
}
