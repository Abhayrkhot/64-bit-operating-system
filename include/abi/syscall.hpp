// System call ABI shared by the kernel and user space.
//
// `syscall` with the number in RAX and arguments in RDI, RSI, RDX, R10, R8,
// R9. The result comes back in RAX: >= 0 on success, -errno on failure. RCX
// and R11 are clobbered (the CPU uses them for the return RIP/RFLAGS).
#pragma once

#include <stdint.h>

namespace os::sys {

enum Number : uint64_t {
  kExit = 0,
  kWrite = 1,
  kRead = 2,
  kYield = 3,
  kGetpid = 4,
  kSleepNs = 5,
  kClockNs = 6,
  kSpawn = 7,    // (path, argv) -> pid
  kWait = 8,     // (pid or -1, int* status) -> pid
  kBrk = 9,      // (new_end or 0) -> current end
  kMmap = 10,    // (length, prot) -> address (anonymous, private)
  kMunmap = 11,  // (address, length)
  kOpen = 12,    // (path, flags) -> fd
  kClose = 13,
  kLseek = 14,   // (fd, offset, whence)
  kStat = 15,    // (path, Stat*)
  kMkdir = 16,
  kUnlink = 17,  // files and empty directories
  kRename = 18,
  kFsync = 19,
  kReaddir = 20,  // (fd, DirEntry*, index) -> 1 if filled, 0 at end
  kSync = 21,
  kGetppid = 22,
  kKill = 23,     // (pid) -> terminate a child
  kSysinfo = 24,  // (SysInfo*)
  kTruncate = 25,  // (path, length)
  kCount
};

enum Errno : int64_t {
  kEPERM = 1, kENOENT = 2, kESRCH = 3, kEINTR = 4, kEIO = 5, kE2BIG = 7, kENOEXEC = 8, kEBADF = 9,
  kECHILD = 10, kEAGAIN = 11, kENOMEM = 12, kEFAULT = 14, kEBUSY = 16, kEEXIST = 17, kENOTDIR = 20,
  kEISDIR = 21, kEINVAL = 22, kEMFILE = 24, kEFBIG = 27, kENOSPC = 28, kESPIPE = 29, kEROFS = 30,
  kENAMETOOLONG = 36, kENOSYS = 38, kENOTEMPTY = 39,
};

// open flags
constexpr uint64_t kORead = 0x0, kOWrite = 0x1, kOReadWrite = 0x2, kOAccessMask = 0x3;
constexpr uint64_t kOCreate = 0x40, kOTruncate = 0x200, kOAppend = 0x400, kODirectory = 0x10000;
// mmap prot
constexpr uint64_t kProtRead = 1, kProtWrite = 2;
// lseek whence
constexpr uint64_t kSeekSet = 0, kSeekCur = 1, kSeekEnd = 2;

// Wait status: exited normally with code c -> c (0..255); killed by a fault
// -> 128 + kSig*.
constexpr int kSigIll = 4, kSigKill = 9, kSigSegv = 11, kSigFpe = 8;

constexpr uint32_t kTypeFile = 1, kTypeDir = 2, kTypeConsole = 3;

struct Stat {
  uint64_t size;
  uint32_t type;
  uint32_t inode;
  uint32_t links;
  uint32_t reserved;
  uint64_t blocks;  // 4 KiB blocks allocated
};

struct DirEntry {
  uint32_t inode;
  uint32_t type;
  char name[56];
};

struct SysInfo {
  uint64_t uptime_ns;
  uint64_t free_bytes;
  uint64_t total_bytes;
  uint64_t processes;
  uint64_t context_switches;
  uint64_t tsc_hz;
};

constexpr uint64_t kMaxPath = 256;
constexpr uint64_t kMaxArgBytes = 4096;  // argv strings + pointers on the initial stack
constexpr uint64_t kMaxArgs = 64;

}  // namespace os::sys
