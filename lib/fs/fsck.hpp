// Offline consistency checker for the file system. Deliberately independent of
// filesystem.cpp (it shares only format.hpp), so a bug in the file system can't
// hide behind the same bug in its checker. Read-only.
//
// Run it after recovery (mount + unmount) to check a crashed image. Checks:
//   superblock: a valid slot exists; layout fits the device
//   inodes:     bitmap bit <=> inode in use; type valid; block count matches
//   blocks:     every pointer inside the data region, referenced at most once,
//               allocated in the bitmap; no allocated-but-unreferenced blocks;
//               nothing mapped past end of file
//   tree:       every directory reachable once from the root, parent pointers
//               correct, entries point at live inodes of the stated type,
//               names valid and unique per directory, link counts exact,
//               no orphaned inodes
#pragma once

#include <stdint.h>

#include "fs/blockdev.hpp"

namespace os::fs {

struct FsckReport {
  static constexpr int kMaxMessages = 12;
  uint32_t errors = 0;
  uint32_t files = 0, dirs = 0;
  uint64_t used_blocks = 0, file_bytes = 0;
  bool clean_flag = false;
  char messages[kMaxMessages][112] = {};
};

// Returns 0 if the check ran (inspect report.errors), -errno if the image
// couldn't be read at all.
int fsck(BlockDevice* dev, FsckReport* report);

}  // namespace os::fs
