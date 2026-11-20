// System call dispatch. Every user pointer goes through usercopy; data moves
// through the process's kernel I/O buffer, so file backends never see user
// memory.
#include <abi/syscall.hpp>

#include "arch/x86_64/cpu.hpp"
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/interrupts.hpp"
#include "common/mem.hpp"
#include "core/cmdline.hpp"
#include "core/log.hpp"
#include "core/time.hpp"
#include "fs/vfs.hpp"
#include "mm/kheap.hpp"
#include "mm/pmm.hpp"
#include "mm/vmm.hpp"
#include "proc/process.hpp"
#include "proc/syscall.hpp"
#include "proc/usercopy.hpp"

extern "C" void syscall_entry();

namespace {

bool g_force_iret;  // `sysret=0` on the command line: always return with iretq

using namespace os::sys;
using interrupts::Frame;

constexpr uint64_t kIoChunk = 4096;
constexpr uint64_t kMaxIo = 1ull << 30;

struct PathBuf {
  char s[kMaxPath];
};

int64_t get_path(uint64_t user, PathBuf* out) {
  int64_t n = usercopy::strncpy_from_user(out->s, user, sizeof(out->s));
  if (n < 0) return n;
  if (n == 0) return -kENOENT;
  return 0;
}

int64_t sys_write(Process* p, int fd, uint64_t buf, uint64_t len) {
  File* f = process::get_fd(p, fd);
  if (!f) return -kEBADF;
  if ((f->flags & kOAccessMask) == kORead) return -kEBADF;
  if (len > kMaxIo) return -kEINVAL;
  if (!usercopy::range_ok(buf, len)) return -kEFAULT;
  auto* kbuf = static_cast<uint8_t*>(kmalloc(kIoChunk));
  if (!kbuf) return -kENOMEM;
  uint64_t done = 0;
  int64_t r = 0;
  while (done < len) {
    uint64_t chunk = os::min(kIoChunk, len - done);
    if ((r = usercopy::from_user(kbuf, buf + done, chunk)) < 0) break;
    r = f->ops->write(f, kbuf, chunk);
    if (r <= 0) break;
    done += static_cast<uint64_t>(r);
    if (static_cast<uint64_t>(r) < chunk) break;
  }
  kfree(kbuf);
  return done ? static_cast<int64_t>(done) : r;
}

int64_t sys_read(Process* p, int fd, uint64_t buf, uint64_t len) {
  File* f = process::get_fd(p, fd);
  if (!f) return -kEBADF;
  if ((f->flags & kOAccessMask) == kOWrite) return -kEBADF;
  if (len > kMaxIo) return -kEINVAL;
  if (!usercopy::range_ok(buf, len)) return -kEFAULT;
  auto* kbuf = static_cast<uint8_t*>(kmalloc(kIoChunk));
  if (!kbuf) return -kENOMEM;
  uint64_t done = 0;
  int64_t r = 0;
  while (done < len) {
    uint64_t chunk = os::min(kIoChunk, len - done);
    r = f->ops->read(f, kbuf, chunk);
    if (r <= 0) break;
    if (usercopy::to_user(buf + done, kbuf, static_cast<uint64_t>(r)) < 0) {
      r = -kEFAULT;
      break;
    }
    done += static_cast<uint64_t>(r);
    if (static_cast<uint64_t>(r) < chunk || f->type == kTypeConsole) break;
  }
  kfree(kbuf);
  return done ? static_cast<int64_t>(done) : r;
}

int64_t sys_spawn(Process* p, uint64_t upath, uint64_t uargv) {
  PathBuf path;
  int64_t r = get_path(upath, &path);
  if (r < 0) return r;
  auto* strings = static_cast<char*>(kmalloc(kMaxArgBytes));
  if (!strings) return -kENOMEM;
  const char* argv[kMaxArgs + 1];
  uint64_t argc = 0, used = 0;
  if (!uargv) {
    argv[argc++] = path.s;
  } else {
    for (;;) {
      uint64_t ptr;
      if ((r = usercopy::from_user(&ptr, uargv + argc * 8, 8)) < 0) goto out;
      if (!ptr) break;
      if (argc == kMaxArgs) {
        r = -kE2BIG;
        goto out;
      }
      int64_t n = usercopy::strncpy_from_user(strings + used, ptr, kMaxArgBytes - used);
      if (n < 0) {
        r = n == -kENAMETOOLONG ? -kE2BIG : n;
        goto out;
      }
      argv[argc++] = strings + used;
      used += static_cast<uint64_t>(n) + 1;
    }
  }
  argv[argc] = nullptr;
  r = process::spawn(path.s, argv, p);
out:
  kfree(strings);
  return r;
}

int64_t sys_wait(Process* p, int64_t pid, uint64_t ustatus) {
  if (ustatus && !usercopy::range_ok(ustatus, sizeof(int))) return -kEFAULT;
  int status = 0;
  int r = process::wait(p, pid < 0 ? -1 : static_cast<int>(pid), &status);
  if (r > 0 && ustatus && usercopy::to_user(ustatus, &status, sizeof(status)) < 0) return -kEFAULT;
  return r;
}

int64_t sys_open(Process* p, uint64_t upath, uint64_t flags) {
  PathBuf path;
  int64_t r = get_path(upath, &path);
  if (r < 0) return r;
  File* f;
  if ((r = vfs::open(path.s, flags, &f)) < 0) return r;
  r = process::install_fd(p, f);
  if (r < 0) file_put(f);
  return r;
}

int64_t sys_lseek(Process* p, int fd, int64_t offset, uint64_t whence) {
  File* f = process::get_fd(p, fd);
  if (!f) return -kEBADF;
  if (!f->ops->seekable) return -kESPIPE;
  int64_t base;
  if (whence == kSeekSet) {
    base = 0;
  } else if (whence == kSeekCur) {
    base = static_cast<int64_t>(f->offset);
  } else if (whence == kSeekEnd) {
    Stat st;
    int64_t r = f->ops->stat(f, &st);
    if (r < 0) return r;
    base = static_cast<int64_t>(st.size);
  } else {
    return -kEINVAL;
  }
  int64_t pos;
  if (__builtin_add_overflow(base, offset, &pos) || pos < 0) return -kEINVAL;
  f->offset = static_cast<uint64_t>(pos);
  return pos;
}

int64_t sys_stat(uint64_t upath, uint64_t ust) {
  PathBuf path;
  int64_t r = get_path(upath, &path);
  if (r < 0) return r;
  if (!usercopy::range_ok(ust, sizeof(Stat))) return -kEFAULT;
  Stat st;
  if ((r = vfs::stat(path.s, &st)) < 0) return r;
  return usercopy::to_user(ust, &st, sizeof(st));
}

int64_t sys_readdir(Process* p, int fd, uint64_t uent, uint64_t index) {
  File* f = process::get_fd(p, fd);
  if (!f) return -kEBADF;
  if (!f->ops->readdir) return -kENOTDIR;
  if (!usercopy::range_ok(uent, sizeof(DirEntry))) return -kEFAULT;
  DirEntry e;
  int64_t r = f->ops->readdir(f, index, &e);
  if (r == 1 && usercopy::to_user(uent, &e, sizeof(e)) < 0) return -kEFAULT;
  return r;
}

int64_t sys_two_paths(uint64_t a, uint64_t b) {
  PathBuf from, to;
  int64_t r = get_path(a, &from);
  if (r < 0) return r;
  if ((r = get_path(b, &to)) < 0) return r;
  return vfs::rename(from.s, to.s);
}

int64_t sys_sysinfo(uint64_t uinfo) {
  SysInfo info{};
  info.uptime_ns = time::now_ns();
  info.free_bytes = pmm::free_bytes();
  info.total_bytes = pmm::managed_bytes();
  info.processes = process::count();
  info.context_switches = sched::stats().context_switches;
  info.tsc_hz = time::tsc_hz();
  return usercopy::to_user(uinfo, &info, sizeof(info));
}

int64_t dispatch(Process* p, Frame* f) {
  uint64_t a0 = f->rdi, a1 = f->rsi, a2 = f->rdx;
  int fd = static_cast<int>(a0);
  if (a0 > 0x7FFFFFFF) fd = -1;
  switch (f->rax) {
    case kExit: process::exit(static_cast<int>(a0 & 0xFF));
    case kWrite: return sys_write(p, fd, a1, a2);
    case kRead: return sys_read(p, fd, a1, a2);
    case kYield: sched::yield(); return 0;
    case kGetpid: return p->pid;
    case kGetppid: return p->parent ? p->parent->pid : 0;
    case kSleepNs: sched::sleep_ns(os::min<uint64_t>(a0, 60ull * 1000000000ull)); return 0;
    case kClockNs: return static_cast<int64_t>(time::now_ns());
    case kSpawn: return sys_spawn(p, a0, a1);
    case kWait: return sys_wait(p, static_cast<int64_t>(a0), a1);
    case kBrk: return static_cast<int64_t>(process::brk(p, a0));
    case kMmap: return process::mmap(p, a0, a1);
    case kMunmap: return process::munmap(p, a0, a1);
    case kOpen: return sys_open(p, a0, a1);
    case kClose: return process::close_fd(p, fd);
    case kLseek: return sys_lseek(p, fd, static_cast<int64_t>(a1), a2);
    case kStat: return sys_stat(a0, a1);
    case kMkdir: {
      PathBuf path;
      int64_t r = get_path(a0, &path);
      return r < 0 ? r : vfs::mkdir(path.s);
    }
    case kUnlink: {
      PathBuf path;
      int64_t r = get_path(a0, &path);
      return r < 0 ? r : vfs::unlink(path.s);
    }
    case kRename: return sys_two_paths(a0, a1);
    case kTruncate: {
      PathBuf path;
      int64_t r = get_path(a0, &path);
      return r < 0 ? r : vfs::truncate(path.s, a1);
    }
    case kFsync: {
      File* file = process::get_fd(p, fd);
      if (!file) return -kEBADF;
      return file->ops->fsync ? file->ops->fsync(file) : 0;
    }
    case kReaddir: return sys_readdir(p, fd, a1, a2);
    case kSync: return vfs::sync();
    case kKill: return process::kill(p, static_cast<int>(a0));
    case kSysinfo: return sys_sysinfo(a0);
    default: return -kENOSYS;
  }
}

}  // namespace

namespace syscall {

void init() {
  arch::wrmsr(arch::kMsrEfer, arch::rdmsr(arch::kMsrEfer) | 1);  // SCE
  // SYSRET loads CS = STAR[63:48] + 16 and SS = STAR[63:48] + 8. Intel CPUs
  // force RPL 3 on both; AMD CPUs take the RPL bits from STAR as written, so
  // the base must carry RPL 3 itself (as Linux does with __USER32_CS).
  // Without it, user code on AMD runs with CS/SS RPL 0, the next interrupt
  // looks like it came from the kernel, and its iretq faults on SS.
  arch::wrmsr(arch::kMsrStar, (static_cast<uint64_t>(gdt::kUserBase | 3) << 48) |
                                  (static_cast<uint64_t>(gdt::kKernelCode) << 32));
  arch::wrmsr(arch::kMsrLstar, reinterpret_cast<uint64_t>(syscall_entry));
  arch::wrmsr(arch::kMsrFmask, 0x47700);  // IF, TF, DF, NT, AC
  usercopy::init();
  char v[8];
  g_force_iret = cmdline::get("sysret", v, sizeof(v)) && v[0] == '0';
}

}  // namespace syscall

extern "C" uint64_t syscall_dispatch(Frame* f) {
  arch::sti();
  Process* p = process::current();
  f->rax = static_cast<uint64_t>(dispatch(p, f));
  if (p->killed) process::exit(128 + kSigKill);
  arch::cli();
  // User code can't change RFLAGS' privileged bits, but keep the frame clean
  // anyway: IF on; IOPL, NT, RF, VM, VIF, VIP off.
  f->rflags = (f->rflags & ~0x1B7000ull) | 0x202;
  // sysret with a non-canonical RCX faults in ring 0 on Intel CPUs; only take
  // the fast path for a plain user RIP.
  return !g_force_iret && f->rip < vmm::kUserEnd && f->cs == gdt::kUserCode;
}
