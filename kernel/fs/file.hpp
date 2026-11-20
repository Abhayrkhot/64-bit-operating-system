// Open file objects (what a file descriptor refers to). Each backend fills in
// a FileOps table; syscalls move data through kernel buffers and do the
// user-copy themselves, so backends only see kernel memory.
#pragma once

#include <abi/syscall.hpp>
#include <stdint.h>

struct File;

struct FileOps {
  int64_t (*read)(File* f, uint8_t* buf, uint64_t n);
  int64_t (*write)(File* f, const uint8_t* buf, uint64_t n);
  int64_t (*stat)(File* f, os::sys::Stat* st);
  // Fills `out` with entry `index`; returns 1, 0 at the end, or -errno.
  int64_t (*readdir)(File* f, uint64_t index, os::sys::DirEntry* out);
  int64_t (*fsync)(File* f);
  void (*release)(File* f);  // last reference dropped
  bool seekable;
};

struct File {
  const FileOps* ops;
  uint32_t refs;
  uint32_t type;  // os::sys::kType*
  uint64_t offset;
  uint64_t flags;  // open flags
  void* priv;
  uint64_t priv2;
};

File* file_alloc(const FileOps* ops, uint32_t type, uint64_t flags, void* priv);
File* file_get(File* f);  // takes a reference
void file_put(File* f);   // drops one; releases at zero
