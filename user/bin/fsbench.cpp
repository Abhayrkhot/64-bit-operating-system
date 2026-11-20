// File-system benchmarks from user space, printed as BENCH lines.
//
//   fsync_4k          write 4 KiB to a new file + fsync, per file (latency)
//   group_commit      256 small appends then one fsync, vs fsync after each
//   seq_write/read    16 MiB in 64 KiB writes + fsync, then read back
//   metadata          create + unlink of empty files (ops/s)
//
// Under QEMU the virtio disk sits on the host's page cache and a flush is a
// host fdatasync, so absolute numbers depend on the host's storage.
#include "libc.hpp"
#include "util/sort.hpp"

namespace {

uint64_t g_tsc_hz;
uint8_t g_buf[64 * 1024];

uint64_t us(uint64_t ns) { return ns / 1000; }

void report_ns(const char* bench, const char* metric, uint64_t* ns, uint64_t n) {
  os::heap_sort(ns, n);
  uint64_t sum = 0;
  for (uint64_t i = 0; i < n; i++) sum += ns[i];
  printf("BENCH %s %s n=%lu mean_us=%lu p50_us=%lu p99_us=%lu\n", bench, metric, n, us(sum / n), us(ns[n / 2]),
         us(ns[n * 99 / 100]));
}

}  // namespace

int main(int, char**) {
  sys::SysInfo info;
  sysinfo(&info);
  g_tsc_hz = info.tsc_hz;
  for (size_t i = 0; i < sizeof(g_buf); i++) g_buf[i] = static_cast<uint8_t>(i * 31);
  mkdir("/bench");

  // fsync latency for a fresh 4 KiB file (create + data + metadata commit).
  constexpr int kFiles = 300;
  uint64_t lat[kFiles];
  for (int i = 0; i < kFiles; i++) {
    char p[40];
    snprintf(p, sizeof(p), "/bench/s%03d", i);
    uint64_t t0 = clock_ns();
    int fd = open(p, sys::kOWrite | sys::kOCreate | sys::kOTruncate);
    write(fd, g_buf, 4096);
    fsync(fd);
    close(fd);
    lat[i] = clock_ns() - t0;
  }
  report_ns("fs", "fsync_4k_new_file", lat, kFiles);

  // Group commit: many small appends share one journal commit.
  constexpr int kAppends = 256;
  int fd = open("/bench/log", sys::kOWrite | sys::kOCreate | sys::kOTruncate);
  uint64_t t0 = clock_ns();
  for (int i = 0; i < kAppends; i++) {
    write(fd, g_buf, 512);
    fsync(fd);
  }
  uint64_t each = clock_ns() - t0;
  t0 = clock_ns();
  for (int i = 0; i < kAppends; i++) write(fd, g_buf, 512);
  fsync(fd);
  uint64_t grouped = clock_ns() - t0;
  close(fd);
  printf("BENCH fs group_commit appends=%d fsync_each_us=%lu one_fsync_us=%lu speedup_x10=%lu\n", kAppends, us(each),
         us(grouped), grouped ? each * 10 / grouped : 0);

  // Sequential throughput.
  constexpr uint64_t kBytes = 16ull << 20;
  fd = open("/bench/seq", sys::kOWrite | sys::kOCreate | sys::kOTruncate);
  t0 = clock_ns();
  for (uint64_t done = 0; done < kBytes; done += sizeof(g_buf)) write(fd, g_buf, sizeof(g_buf));
  fsync(fd);
  uint64_t wns = clock_ns() - t0;
  close(fd);
  fd = open("/bench/seq", 0);
  t0 = clock_ns();
  for (uint64_t done = 0; done < kBytes; done += sizeof(g_buf)) read(fd, g_buf, sizeof(g_buf));
  uint64_t rns = clock_ns() - t0;
  close(fd);
  // The file is twice the 8 MiB buffer cache, so the read goes to the disk,
  // one synchronous 4 KiB request at a time (no read-ahead yet).
  printf("BENCH fs seq_write MiB_per_s=%lu\n", static_cast<unsigned long>((kBytes >> 20) * 1000000000ull / (wns ? wns : 1)));
  printf("BENCH fs seq_read_uncached MiB_per_s=%lu\n",
         static_cast<unsigned long>((kBytes >> 20) * 1000000000ull / (rns ? rns : 1)));
  unlink("/bench/seq");

  // Metadata operations (no fsync: they batch into periodic commits).
  constexpr int kMeta = 2000;
  mkdir("/bench/m");
  t0 = clock_ns();
  for (int i = 0; i < kMeta; i++) {
    char p[40];
    snprintf(p, sizeof(p), "/bench/m/%d", i);
    close(open(p, sys::kOWrite | sys::kOCreate));
  }
  uint64_t cns = clock_ns() - t0;
  t0 = clock_ns();
  for (int i = 0; i < kMeta; i++) {
    char p[40];
    snprintf(p, sizeof(p), "/bench/m/%d", i);
    unlink(p);
  }
  uint64_t uns = clock_ns() - t0;
  sync();
  printf("BENCH fs metadata create_ops_per_s=%lu unlink_ops_per_s=%lu\n",
         static_cast<unsigned long>(kMeta * 1000000000ull / (cns ? cns : 1)),
         static_cast<unsigned long>(kMeta * 1000000000ull / (uns ? uns : 1)));
  return 0;
}
