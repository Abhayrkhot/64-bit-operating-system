// Path resolution across the two file systems:
//   /bin/...  the read-only initrd (programs)
//   /...      ShaleFS on the virtio disk, once mounted (M4)
#pragma once

#include <shale/syscall.hpp>
#include <stdint.h>

#include "fs/file.hpp"

namespace vfs {

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

}  // namespace vfs
