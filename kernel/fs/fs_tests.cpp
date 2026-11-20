// File-system tests in the kernel: the file system on the virtio disk (qemu.py
// --data-disk attaches a blank one, which vfs::init formats).
#include "core/log.hpp"
#include "fmt/format.hpp"
#include "fs/vfs.hpp"
#include "proc/process.hpp"
#include "test/ktest.hpp"
#include "util/random.hpp"

namespace {

int run_user(const char* const* argv) {
  char path[64];
  os::fmt::format(path, sizeof(path), "/bin/%s", argv[0]);
  int pid = process::spawn(path, argv, process::kernel());
  if (pid < 0) return pid - 1000;
  int status = -1;
  process::wait(process::kernel(), pid, &status);
  return status;
}

bool fsck_clean() {
  os::fs::FsckReport rep;
  if (vfs::check(&rep) != 0) return false;
  for (uint32_t i = 0; i < rep.errors && i < os::fs::FsckReport::kMaxMessages; i++)
    kprintf("    fsck: %s\n", rep.messages[i]);
  return rep.errors == 0;
}

int write_file(const char* path, const uint8_t* data, uint64_t n) {
  File* f;
  int r = vfs::open(path, os::sys::kOWrite | os::sys::kOCreate | os::sys::kOTruncate, &f);
  if (r < 0) return r;
  int64_t w = f->ops->write(f, data, n);
  file_put(f);
  return w == static_cast<int64_t>(n) ? 0 : -1;
}

bool file_equals(const char* path, const uint8_t* data, uint64_t n) {
  File* f;
  if (vfs::open(path, os::sys::kORead, &f) < 0) return false;
  static uint8_t buf[64 * 1024];
  int64_t r = f->ops->read(f, buf, sizeof(buf));
  file_put(f);
  return r == static_cast<int64_t>(n) && memcmp(buf, data, n) == 0;
}

}  // namespace

KTEST(fs_root_is_mounted) { KEXPECT(vfs::root_mounted()); }

KTEST(fs_user_syscall_semantics) {
  if (!vfs::root_mounted()) return;
  const char* a[] = {"fstest", nullptr};
  KEXPECT_EQ(run_user(a), 0);
  KEXPECT(fsck_clean());
}

KTEST(fs_remount_persists_and_fscks_clean) {
  if (!vfs::root_mounted()) return;
  static uint8_t data[40000];
  os::Random rng(5);
  KEXPECT_EQ(vfs::mkdir("/persist"), 0);
  for (int i = 0; i < 40; i++) {
    char p[32];
    os::fmt::format(p, sizeof(p), "/persist/f%d", i);
    for (uint64_t k = 0; k < sizeof(data); k++) data[k] = static_cast<uint8_t>(rng.next() + i);
    KEXPECT_EQ(write_file(p, data, 1000 + 900 * static_cast<uint64_t>(i)), 0);
  }
  KEXPECT_EQ(vfs::remount(), 0);
  os::Random again(5);
  for (int i = 0; i < 40; i++) {
    char p[32];
    os::fmt::format(p, sizeof(p), "/persist/f%d", i);
    for (uint64_t k = 0; k < sizeof(data); k++) data[k] = static_cast<uint8_t>(again.next() + i);
    KEXPECT(file_equals(p, data, 1000 + 900 * static_cast<uint64_t>(i)));
  }
  KEXPECT(fsck_clean());
}

KTEST(fs_stress_then_fsck) {
  if (!vfs::root_mounted()) return;
  const char* a[] = {"fsstress", "3", "1500", nullptr};
  KEXPECT_EQ(run_user(a), 0);
  KEXPECT(fsck_clean());
}
