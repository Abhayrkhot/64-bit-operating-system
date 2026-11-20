#include "fs/vfs.hpp"

#include "arch/x86_64/cpu.hpp"
#include "common/mem.hpp"
#include "core/boot.hpp"
#include "core/log.hpp"
#include "core/time.hpp"
#include "drivers/virtio_blk.hpp"
#include "fs/initrd.hpp"
#include "mm/kheap.hpp"
#include "sched/sched.hpp"

namespace vfs {
namespace {

using namespace os::sys;
using os::fs::InodeInfo;
using os::fs::InodeType;
using os::fs::FileSystem;

constexpr uint64_t kMaxExecutable = 16ull << 20;
constexpr uint64_t kCommitIntervalMs = 1000;

os::fs::BlockDevice* g_disk;
FileSystem* g_fs;
Mutex g_lock;

struct Locked {
  Locked() { g_lock.lock(); }
  ~Locked() { g_lock.unlock(); }
};

bool in_bin(const char* path) { return strncmp(path, "/bin/", 5) == 0 || strcmp(path, "/bin") == 0; }

uint64_t now_ns() { return time::now_ns(); }
void fs_log(const char* msg) { kprintf("%s\n", msg); }

// ---- initrd files and the /bin directory --------------------------------------

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

int64_t bin_readdir(File*, uint64_t index, DirEntry* out) {
  const char* name;
  uint64_t size;
  for (uint64_t i = 0, seen = 0; initrd::entry(i, &name, &size); i++) {
    if (strncmp(name, "bin/", 4) != 0) continue;
    if (seen++ == index) {
      *out = {};
      out->type = kTypeFile;
      out->inode = static_cast<uint32_t>(i + 1);
      size_t n = os::min(strlen(name + 4), sizeof(out->name) - 1);
      memcpy(out->name, name + 4, n);
      return 1;
    }
  }
  return 0;
}
int64_t dir_stat(File*, Stat* st) {
  *st = {};
  st->type = kTypeDir;
  st->links = 1;
  return 0;
}
int64_t dir_read(File*, uint8_t*, uint64_t) { return -kEISDIR; }
int64_t dir_write(File*, const uint8_t*, uint64_t) { return -kEISDIR; }
const FileOps kBinDir = {dir_read, dir_write, dir_stat, bin_readdir, nullptr, nullptr, false};

// ---- Disk files: priv2 = inode, priv = generation ----------------------------

uint32_t ino_of(File* f) { return static_cast<uint32_t>(f->priv2); }
bool stale(File* f) { return g_fs->generation(ino_of(f)) != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(f->priv)); }

int64_t fs_read(File* f, uint8_t* buf, uint64_t n) {
  Locked l;
  if (stale(f)) return -kENOENT;
  int64_t r = g_fs->read(ino_of(f), f->offset, buf, n);
  if (r > 0) f->offset += static_cast<uint64_t>(r);
  return r;
}

int64_t fs_write(File* f, const uint8_t* buf, uint64_t n) {
  Locked l;
  if (stale(f)) return -kENOENT;
  if (f->flags & kOAppend) {
    InodeInfo info;
    if (g_fs->stat(ino_of(f), &info) == 0) f->offset = info.size;
  }
  int64_t r = g_fs->write(ino_of(f), f->offset, buf, n);
  if (r > 0) f->offset += static_cast<uint64_t>(r);
  return r;
}

void fill_stat(uint32_t ino, const InodeInfo& info, Stat* st) {
  *st = {};
  st->size = info.size;
  st->type = info.type == InodeType::kDir ? kTypeDir : kTypeFile;
  st->inode = ino;
  st->links = info.links;
  st->blocks = info.blocks;
}

int64_t fs_stat(File* f, Stat* st) {
  Locked l;
  if (stale(f)) return -kENOENT;
  InodeInfo info;
  int r = g_fs->stat(ino_of(f), &info);
  if (r == 0) fill_stat(ino_of(f), info, st);
  return r;
}

int64_t fs_readdir(File* f, uint64_t index, DirEntry* out) {
  Locked l;
  if (stale(f)) return -kENOENT;
  // The root also shows the /bin mount point.
  if (ino_of(f) == os::fs::kRootInode) {
    if (index == 0) {
      *out = {};
      out->type = kTypeDir;
      memcpy(out->name, "bin", 4);
      return 1;
    }
    index--;
  }
  char name[58];
  uint32_t child;
  InodeType type;
  int r = g_fs->readdir(ino_of(f), index, name, &child, &type);
  if (r == 1) {
    *out = {};
    out->inode = child;
    out->type = type == InodeType::kDir ? kTypeDir : kTypeFile;
    memcpy(out->name, name, strlen(name) + 1);
  }
  return r;
}

int64_t fs_fsync(File* f) {
  Locked l;
  return g_fs->fsync(ino_of(f));
}

const FileOps kFsFile = {fs_read, fs_write, fs_stat, nullptr, fs_fsync, nullptr, true};
const FileOps kFsDir = {dir_read, dir_write, fs_stat, fs_readdir, fs_fsync, nullptr, false};

void commit_thread(void*) {
  for (;;) {
    sched::sleep_ms(kCommitIntervalMs);
    Locked l;
    if (g_fs) g_fs->sync();
  }
}

bool disk_blank(os::fs::BlockDevice* d) {
  auto* blk = static_cast<uint8_t*>(kmalloc(os::fs::kBlockSize));
  bool blank = true;
  for (uint32_t b = 0; b < 2 && blank; b++) {
    if (d->read(b, 1, blk) < 0) blank = false;
    for (uint32_t i = 0; i < os::fs::kBlockSize && blank; i++) blank = blk[i] == 0;
  }
  kfree(blk);
  return blank;
}

}  // namespace

void init() {
  g_disk = virtio_blk::probe();
  if (!g_disk) {
    kprintf("vfs: no disk; / is empty, /bin is the initrd\n");
    return;
  }
  g_fs = new FileSystem();
  FileSystem::Options o;
  o.cache_blocks = 2048;  // 8 MiB
  o.max_txn_blocks = 256;
  o.now_ns = now_ns;
  o.log = fs_log;
  int r = g_fs->mount(g_disk, o);
  if (r == -kEINVAL && disk_blank(g_disk)) {
    FileSystem::MkfsOptions mo;
    mo.uuid = arch::rdtsc() ^ 0x5348414c45ull;
    kprintf("vfs: blank disk, formatting\n");
    if ((r = FileSystem::mkfs(g_disk, mo)) == 0) r = g_fs->mount(g_disk, o);
  }
  if (r < 0) {
    kprintf("vfs: mount failed (%d)\n", r);
    delete g_fs;
    g_fs = nullptr;
    return;
  }
  const auto& sb = g_fs->superblock();
  const auto& st = g_fs->stats();
  kprintf("vfs: file system mounted at / (%u blocks, journal %u, %u free; recovery replayed %lu txns / %lu blocks)\n",
          sb.total_blocks, sb.journal_blocks, g_fs->free_blocks(), st.replayed_txns, st.replayed_blocks);
  sched::spawn("fs-commit", commit_thread, nullptr);
}

void shutdown() {
  if (!g_fs) return;
  Locked l;
  int r = g_fs->unmount();
  kprintf("vfs: unmounted (%d)\n", r);
  delete g_fs;
  g_fs = nullptr;
}

bool root_mounted() { return g_fs != nullptr; }

int check(os::fs::FsckReport* report) {
  if (!g_fs || !g_disk) return -kENOENT;
  Locked l;
  int r = g_fs->unmount();
  if (r < 0) return r;
  r = os::fs::fsck(g_disk, report);
  delete g_fs;
  g_fs = new FileSystem();
  FileSystem::Options o;
  o.cache_blocks = 2048;
  o.max_txn_blocks = 256;
  o.now_ns = now_ns;
  o.log = fs_log;
  int m = g_fs->mount(g_disk, o);
  return r < 0 ? r : m;
}

int remount() {
  if (!g_fs || !g_disk) return -kENOENT;
  Locked l;
  int r = g_fs->unmount();
  if (r < 0) return r;
  delete g_fs;
  g_fs = new FileSystem();
  FileSystem::Options o;
  o.cache_blocks = 2048;
  o.max_txn_blocks = 256;
  o.now_ns = now_ns;
  o.log = fs_log;
  r = g_fs->mount(g_disk, o);
  if (r < 0) {
    delete g_fs;
    g_fs = nullptr;
  }
  return r;
}
FileSystem* fs() { return g_fs; }
os::fs::BlockDevice* disk() { return g_disk; }
void lock() { g_lock.lock(); }
void unlock() { g_lock.unlock(); }

int load_executable(const char* path, const uint8_t** data, uint64_t* size) {
  if (in_bin(path)) return initrd::find(path, data, size) ? 0 : -kENOENT;
  if (!g_fs) return -kENOENT;
  Locked l;
  uint32_t ino;
  int r = g_fs->lookup(path, &ino);
  if (r < 0) return r;
  InodeInfo info;
  if ((r = g_fs->stat(ino, &info)) < 0) return r;
  if (info.type != InodeType::kFile) return -kEISDIR;
  if (info.size > kMaxExecutable) return -kENOEXEC;
  auto* buf = static_cast<uint8_t*>(kmalloc(info.size ? info.size : 1));
  if (!buf) return -kENOMEM;
  if (g_fs->read(ino, 0, buf, info.size) != static_cast<int64_t>(info.size)) {
    kfree(buf);
    return -kEIO;
  }
  *data = buf;
  *size = info.size;
  return 0;
}

void release_executable(const uint8_t* data) {
  // initrd images live in boot memory; heap copies were made above.
  const auto& bi = boot::info();
  auto p = reinterpret_cast<uint64_t>(data);
  uint64_t lo = os::kHhdmBase + bi.initrd_phys, hi = lo + bi.initrd_size;
  if (p < lo || p >= hi) kfree(const_cast<uint8_t*>(data));
}

int open(const char* path, uint64_t flags, File** out) {
  if (path[0] != '/') return -kENOENT;
  uint64_t access = flags & kOAccessMask;
  if (strcmp(path, "/bin") == 0 || strcmp(path, "/bin/") == 0) {
    if (access != kORead) return -kEISDIR;
    *out = file_alloc(&kBinDir, kTypeDir, flags, nullptr);
    return *out ? 0 : -kENOMEM;
  }
  if (in_bin(path)) {
    const uint8_t* data;
    uint64_t size;
    if (!initrd::find(path, &data, &size)) return (flags & kOCreate) ? -kEROFS : -kENOENT;
    if (access != kORead || (flags & kOTruncate)) return -kEROFS;
    if (flags & kODirectory) return -kENOTDIR;
    File* f = file_alloc(&kInitrdFile, kTypeFile, flags, const_cast<uint8_t*>(data));
    if (!f) return -kENOMEM;
    f->priv2 = size;
    *out = f;
    return 0;
  }
  if (!g_fs) return (flags & kOCreate) ? -kEROFS : -kENOENT;
  if (access == kOAccessMask) return -kEINVAL;
  Locked l;
  uint32_t ino;
  int r = g_fs->lookup(path, &ino);
  if (r == -kENOENT && (flags & kOCreate)) r = g_fs->create(path, false, &ino);
  if (r < 0) return r;
  InodeInfo info;
  if ((r = g_fs->stat(ino, &info)) < 0) return r;
  bool dir = info.type == InodeType::kDir;
  if (dir && access != kORead) return -kEISDIR;
  if (!dir && (flags & kODirectory)) return -kENOTDIR;
  if (!dir && (flags & kOTruncate) && access != kORead && (r = g_fs->truncate(ino, 0)) < 0) return r;
  File* f = file_alloc(dir ? &kFsDir : &kFsFile, dir ? kTypeDir : kTypeFile, flags,
                       reinterpret_cast<void*>(static_cast<uintptr_t>(g_fs->generation(ino))));
  if (!f) return -kENOMEM;
  f->priv2 = ino;
  *out = f;
  return 0;
}

int stat(const char* path, Stat* st) {
  if (in_bin(path) || !g_fs) {
    File* f;
    int r = open(path, kORead, &f);
    if (r < 0) return r;
    r = static_cast<int>(f->ops->stat(f, st));
    file_put(f);
    return r;
  }
  Locked l;
  uint32_t ino;
  int r = g_fs->lookup(path, &ino);
  if (r < 0) return r;
  InodeInfo info;
  if ((r = g_fs->stat(ino, &info)) == 0) fill_stat(ino, info, st);
  return r;
}

int mkdir(const char* path) {
  if (in_bin(path)) return -kEROFS;
  if (!g_fs) return -kEROFS;
  Locked l;
  return g_fs->mkdir(path);
}

int unlink(const char* path) {
  if (in_bin(path)) return -kEROFS;
  if (!g_fs) return -kENOENT;
  Locked l;
  return g_fs->unlink(path);
}

int rename(const char* from, const char* to) {
  if (in_bin(from) || in_bin(to)) return -kEROFS;
  if (!g_fs) return -kENOENT;
  Locked l;
  return g_fs->rename(from, to);
}

int truncate(const char* path, uint64_t length) {
  if (in_bin(path)) return -kEROFS;
  if (!g_fs) return -kENOENT;
  Locked l;
  uint32_t ino;
  int r = g_fs->lookup(path, &ino);
  return r < 0 ? r : g_fs->truncate(ino, length);
}

int sync() {
  if (!g_fs) return 0;
  Locked l;
  return g_fs->sync();
}

}  // namespace vfs
