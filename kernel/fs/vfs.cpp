#include "fs/vfs.hpp"

#include "common/mem.hpp"
#include "fs/initrd.hpp"

namespace vfs {
namespace {

using namespace shale::sys;

bool in_bin(const char* path) { return strncmp(path, "/bin/", 5) == 0 || strcmp(path, "/bin") == 0; }

// Read-only initrd files: priv = data, priv2 = size.
int64_t rd_read(File* f, uint8_t* buf, uint64_t n) {
  if (f->offset >= f->priv2) return 0;
  uint64_t take = f->priv2 - f->offset < n ? f->priv2 - f->offset : n;
  memcpy(buf, static_cast<const uint8_t*>(f->priv) + f->offset, take);
  f->offset += take;
  return static_cast<int64_t>(take);
}
int64_t rd_write(File*, const uint8_t*, uint64_t) { return -kEROFS; }
int64_t rd_stat(File* f, Stat* st) {
  *st = {};
  st->size = f->priv2;
  st->type = kTypeFile;
  st->links = 1;
  return 0;
}
const FileOps kInitrdFile = {rd_read, rd_write, rd_stat, nullptr, nullptr, nullptr, true};

// The /bin directory: lists initrd entries under bin/.
int64_t bin_readdir(File*, uint64_t index, DirEntry* out) {
  const char* name;
  uint64_t size;
  for (uint64_t i = 0, seen = 0; initrd::entry(i, &name, &size); i++) {
    if (strncmp(name, "bin/", 4) != 0) continue;
    if (seen++ == index) {
      *out = {};
      out->type = kTypeFile;
      out->inode = static_cast<uint32_t>(i + 1);
      size_t n = strlen(name + 4) < sizeof(out->name) - 1 ? strlen(name + 4) : sizeof(out->name) - 1;
      memcpy(out->name, name + 4, n);
      return 1;
    }
  }
  return 0;
}
int64_t dir_stat(File*, Stat* st) {
  *st = {};
  st->type = kTypeDir;
  st->links = 2;
  return 0;
}
int64_t dir_rw(File*, uint8_t*, uint64_t) { return -kEISDIR; }
int64_t dir_write(File*, const uint8_t*, uint64_t) { return -kEISDIR; }
const FileOps kBinDir = {dir_rw, dir_write, dir_stat, bin_readdir, nullptr, nullptr, false};

}  // namespace

int load_executable(const char* path, const uint8_t** data, uint64_t* size) {
  if (in_bin(path) && initrd::find(path, data, size)) return 0;
  return -kENOENT;
}

void release_executable(const uint8_t*) {}

int open(const char* path, uint64_t flags, File** out) {
  if (path[0] != '/') return -kENOENT;
  if (strcmp(path, "/bin") == 0 || strcmp(path, "/bin/") == 0) {
    if ((flags & kOAccessMask) != kORead) return -kEISDIR;
    *out = file_alloc(&kBinDir, kTypeDir, flags, nullptr);
    return *out ? 0 : -kENOMEM;
  }
  if (in_bin(path)) {
    const uint8_t* data;
    uint64_t size;
    if (!initrd::find(path, &data, &size)) return (flags & kOCreate) ? -kEROFS : -kENOENT;
    if ((flags & kOAccessMask) != kORead || (flags & kOTruncate)) return -kEROFS;
    if (flags & kODirectory) return -kENOTDIR;
    File* f = file_alloc(&kInitrdFile, kTypeFile, flags, const_cast<uint8_t*>(data));
    if (!f) return -kENOMEM;
    f->priv2 = size;
    *out = f;
    return 0;
  }
  return -kENOENT;
}

int stat(const char* path, Stat* st) {
  File* f;
  int r = open(path, kORead, &f);
  if (r < 0) return r;
  r = static_cast<int>(f->ops->stat(f, st));
  file_put(f);
  return r;
}

int mkdir(const char* path) { return in_bin(path) ? -kEROFS : -kENOENT; }
int unlink(const char* path) { return in_bin(path) ? -kEROFS : -kENOENT; }
int rename(const char* from, const char* to) { return in_bin(from) || in_bin(to) ? -kEROFS : -kENOENT; }
int truncate(const char* path, uint64_t) { return in_bin(path) ? -kEROFS : -kENOENT; }
int sync() { return 0; }

}  // namespace vfs
