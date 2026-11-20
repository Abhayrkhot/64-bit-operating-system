// Path resolution across the two file systems:
//   /bin/...  the read-only initrd (programs)
//   /...      ShaleFS on the virtio disk
#pragma once

#include <shale/syscall.hpp>
#include <stdint.h>

#include "fs/file.hpp"
#include "fs/fsck.hpp"
#include "fs/shalefs.hpp"

namespace vfs {

// Probes the disk and mounts ShaleFS at / (formatting a completely blank
// disk), then starts the commit thread. Safe to call without a disk.
void init();
// Checkpoints and unmounts (clean shutdown).
void shutdown();
bool root_mounted();
// Unmount and mount again on the same disk (tests: proves on-disk state).
int remount();
// Unmount, run fsck on the disk, mount again. Returns fsck's status.
int check(shale::fs::FsckReport* report);

// Returns a readable image of an executable (initrd memory or a heap copy).
int load_executable(const char* path, const uint8_t** data, uint64_t* size);
void release_executable(const uint8_t* data);

int open(const char* path, uint64_t flags, File** out);
int stat(const char* path, shale::sys::Stat* st);
int mkdir(const char* path);
int unlink(const char* path);
int rename(const char* from, const char* to);
int truncate(const char* path, uint64_t length);
int sync();

// For tests and the `fsck` boot mode: run fn with the file system locked.
shale::fs::ShaleFs* fs();
shale::fs::BlockDevice* disk();
void lock();
void unlock();

}  // namespace vfs
