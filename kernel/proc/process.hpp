// User processes: one address space, one thread, a file table.
//
// User address space layout:
//   0x0000000000400000  program segments (from the ELF, W^X enforced)
//   after the program   brk heap, grows up (sys_brk)
//   0x0000100000000000  anonymous mmap region (sys_mmap), bump allocated
//   0x00007FFFFFF00000  stack top; 256 KiB stack with an unmapped guard below
#pragma once

#include <stdint.h>

#include "fs/file.hpp"
#include "sched/sched.hpp"

namespace vmm {
class AddressSpace;
}

constexpr int kMaxFds = 32;
constexpr unsigned kMaxMmapRegions = 64;

enum class ProcState : uint8_t { kAlive, kZombie };

struct MmapRegion {
  uint64_t start;
  uint64_t pages;
};

struct Process {
  int pid;
  ProcState state;
  bool auto_reap;  // orphan: reaped by the kernel, nobody waits for it
  bool killed;     // terminate at the next return to user mode
  int exit_status;
  char name[32];
  Process* parent;
  Process* first_child;
  Process* sibling;
  Process* all_next;
  vmm::AddressSpace* as;
  Thread* thread;
  WaitQueue child_exited;
  File* fds[kMaxFds];
  uint64_t brk_start, brk_end;
  uint64_t mmap_next;
  MmapRegion regions[kMaxMmapRegions];
  uint64_t pages_mapped;  // brk + mmap + stack, against kPageLimit
  uint64_t entry, user_stack;
  uint64_t argc, argv;
};

namespace process {

constexpr uint64_t kStackTop = 0x00007FFFFFF00000ull;
constexpr uint64_t kStackPages = 64;
constexpr uint64_t kMmapBase = 0x0000100000000000ull;
constexpr uint64_t kPageLimit = 16384;  // 64 MiB of anonymous memory per process

void init();
// The pseudo-process (pid 0) that owns processes the kernel starts.
Process* kernel();
Process* current();

// Loads `path` and starts it as a child of `parent`. argv is a NULL-terminated
// array of kernel strings. Returns the pid or -errno.
int spawn(const char* path, const char* const* argv, Process* parent);
// Reaps a zombie child of `parent` (pid -1: any). Returns its pid and stores
// the wait status, or -ECHILD.
int wait(Process* parent, int pid, int* status);
[[noreturn]] void exit(int status);
int kill(Process* caller, int pid);

uint64_t count();
// fd table helpers
int install_fd(Process* p, File* f);
File* get_fd(Process* p, int fd);
int close_fd(Process* p, int fd);

// Memory syscalls
uint64_t brk(Process* p, uint64_t new_end);
int64_t mmap(Process* p, uint64_t length, uint64_t prot);
int64_t munmap(Process* p, uint64_t addr, uint64_t length);

}  // namespace process
