// ShaleFS functional tests on in-memory devices, checked against a model and
// by fsck after every scenario.
#include <map>
#include <string>
#include <vector>

#include <shale/syscall.hpp>

#include "common/blockdevs.hpp"
#include "common/test.hpp"
#include "fs/fsck.hpp"
#include "fs/shalefs.hpp"
#include "util/random.hpp"

using shale::fs::FsckReport;
using shale::fs::InodeInfo;
using shale::fs::InodeType;
using shale::fs::ShaleFs;

namespace {

ShaleFs::Options small_cache() {
  ShaleFs::Options o;
  o.cache_blocks = 256;  // small enough that eviction paths run
  o.max_txn_blocks = 64;
  return o;
}

bool fsck_clean(shale::fs::BlockDevice* dev) {
  FsckReport rep;
  if (shale::fs::fsck(dev, &rep) < 0) return false;
  for (uint32_t i = 0; i < rep.errors && i < FsckReport::kMaxMessages; i++)
    std::printf("    fsck: %s\n", rep.messages[i]);
  return rep.errors == 0;
}

std::string read_all(ShaleFs& fs, const char* path) {
  uint32_t ino;
  if (fs.lookup(path, &ino) < 0) return "<missing>";
  InodeInfo info;
  fs.stat(ino, &info);
  std::string s(info.size, '\0');
  if (info.size) fs.read(ino, 0, &s[0], info.size);
  return s;
}

int write_file(ShaleFs& fs, const char* path, const std::string& data) {
  uint32_t ino;
  int r = fs.create(path, false, &ino);
  if (r < 0) return r;
  if ((r = fs.truncate(ino, 0)) < 0) return r;
  int64_t w = fs.write(ino, 0, data.data(), data.size());
  return w == static_cast<int64_t>(data.size()) ? 0 : -1;
}

std::string pattern(size_t n, uint64_t seed) {
  shale::Random rng(seed);
  std::string s(n, '\0');
  for (auto& c : s) c = static_cast<char>(rng.next());
  return s;
}

}  // namespace

TEST(fs_mkfs_mount_unmount_clean) {
  host::MemDevice dev(8192);
  EXPECT_EQ(ShaleFs::mkfs(&dev, {}), 0);
  EXPECT(fsck_clean(&dev));
  ShaleFs fs;
  EXPECT_EQ(fs.mount(&dev, small_cache()), 0);
  EXPECT_EQ(fs.unmount(), 0);
  FsckReport rep;
  shale::fs::fsck(&dev, &rep);
  EXPECT(rep.errors == 0 && rep.clean_flag && rep.dirs == 1);
}

TEST(fs_files_dirs_and_persistence) {
  host::MemDevice dev(8192);
  ShaleFs::mkfs(&dev, {});
  {
    ShaleFs fs;
    ASSERT(fs.mount(&dev, small_cache()) == 0);
    EXPECT_EQ(fs.mkdir("/a"), 0);
    EXPECT_EQ(fs.mkdir("/a/b"), 0);
    EXPECT_EQ(fs.mkdir("/a"), -shale::sys::kEEXIST);
    EXPECT_EQ(write_file(fs, "/a/b/hello.txt", "hello, shale"), 0);
    EXPECT_EQ(write_file(fs, "/top", pattern(100000, 1)), 0);
    uint32_t ino;
    EXPECT_EQ(fs.lookup("/a/b/../b/./hello.txt", &ino), 0);
    EXPECT_EQ(fs.lookup("/nope", &ino), -shale::sys::kENOENT);
    EXPECT_EQ(fs.lookup("/top/x", &ino), -shale::sys::kENOTDIR);
    EXPECT_EQ(fs.unmount(), 0);
  }
  EXPECT(fsck_clean(&dev));
  ShaleFs fs;
  ASSERT(fs.mount(&dev, small_cache()) == 0);
  EXPECT(read_all(fs, "/a/b/hello.txt") == "hello, shale");
  EXPECT(read_all(fs, "/top") == pattern(100000, 1));
  uint32_t dir;
  fs.lookup("/a", &dir);
  char name[58];
  uint32_t child;
  InodeType type;
  EXPECT_EQ(fs.readdir(dir, 0, name, &child, &type), 1);
  EXPECT(std::string(name) == "b" && type == InodeType::kDir);
  EXPECT_EQ(fs.readdir(dir, 1, name, &child, &type), 0);
}

TEST(fs_large_files_use_indirect_blocks) {
  host::MemDevice dev(32768);  // 128 MiB
  ShaleFs::mkfs(&dev, {});
  ShaleFs fs;
  ASSERT(fs.mount(&dev, small_cache()) == 0);
  uint32_t ino;
  ASSERT(fs.create("/big", true, &ino) == 0);
  // Past the direct (48 KiB) and single-indirect (4 MiB + 48 KiB) ranges.
  std::string data = pattern(6 << 20, 7);
  EXPECT_EQ(fs.write(ino, 0, data.data(), data.size()), static_cast<int64_t>(data.size()));
  // Sparse write far into the double-indirect range.
  std::string tail = pattern(5000, 8);
  uint64_t far = 64ull << 20;
  EXPECT_EQ(fs.write(ino, far, tail.data(), tail.size()), 5000);
  std::string back(data.size(), '\0');
  EXPECT_EQ(fs.read(ino, 0, &back[0], back.size()), static_cast<int64_t>(back.size()));
  EXPECT(back == data);
  std::string hole(4096, 'x');
  fs.read(ino, 32ull << 20, &hole[0], hole.size());
  EXPECT(hole == std::string(4096, '\0'));
  EXPECT_EQ(fs.sync(), 0);
  EXPECT_EQ(fs.unmount(), 0);
  EXPECT(fsck_clean(&dev));
  // Truncate into the middle of the indirect range, then delete.
  ShaleFs fs2;
  ASSERT(fs2.mount(&dev, small_cache()) == 0);
  uint32_t free0 = fs2.free_blocks();
  EXPECT_EQ(fs2.truncate(ino, 1000000), 0);
  std::string head(1000000, '\0');
  fs2.read(ino, 0, &head[0], head.size());
  EXPECT(head == data.substr(0, 1000000));
  EXPECT(fs2.free_blocks() > free0 + 1000);
  EXPECT_EQ(fs2.unlink("/big"), 0);
  EXPECT_EQ(fs2.unmount(), 0);
  EXPECT(fsck_clean(&dev));
}

TEST(fs_rename_semantics) {
  host::MemDevice dev(8192);
  ShaleFs::mkfs(&dev, {});
  ShaleFs fs;
  ASSERT(fs.mount(&dev, small_cache()) == 0);
  fs.mkdir("/d1");
  fs.mkdir("/d1/sub");
  fs.mkdir("/d2");
  write_file(fs, "/d1/f", "one");
  write_file(fs, "/d2/g", "two");
  EXPECT_EQ(fs.rename("/d1/f", "/d2/g"), 0);  // replace
  EXPECT(read_all(fs, "/d2/g") == "one");
  EXPECT(read_all(fs, "/d1/f") == "<missing>");
  EXPECT_EQ(fs.rename("/d1", "/d1/sub/x"), -shale::sys::kEINVAL);  // into itself
  EXPECT_EQ(fs.rename("/d1/sub", "/d2/sub"), 0);  // move a directory
  uint32_t sub, d2;
  fs.lookup("/d2/sub", &sub);
  fs.lookup("/d2", &d2);
  uint32_t parent;
  EXPECT_EQ(fs.lookup("/d2/sub/..", &parent), 0);
  EXPECT_EQ(parent, d2);
  EXPECT_EQ(fs.rename("/d2/g", "/d2/sub"), -shale::sys::kEISDIR);
  EXPECT_EQ(fs.unlink("/d2"), -shale::sys::kENOTEMPTY);
  EXPECT_EQ(fs.unlink("/d2/sub"), 0);
  EXPECT_EQ(fs.unmount(), 0);
  EXPECT(fsck_clean(&dev));
}

TEST(fs_enospc_is_clean) {
  host::MemDevice dev(1024);  // 4 MiB
  ShaleFs::mkfs(&dev, {});
  ShaleFs fs;
  ASSERT(fs.mount(&dev, small_cache()) == 0);
  uint32_t ino;
  fs.create("/fill", true, &ino);
  std::string chunk = pattern(1 << 20, 3);
  int64_t total = 0, r;
  while ((r = fs.write(ino, static_cast<uint64_t>(total), chunk.data(), chunk.size())) > 0) total += r;
  EXPECT_EQ(r, -shale::sys::kENOSPC);
  EXPECT(total > 1 << 20);
  EXPECT_EQ(fs.unlink("/fill"), 0);
  // The space comes back (after the checkpoint that releases deferred frees).
  EXPECT_EQ(fs.create("/again", true, &ino), 0);
  EXPECT(fs.write(ino, 0, chunk.data(), chunk.size()) == static_cast<int64_t>(chunk.size()));
  EXPECT_EQ(fs.unmount(), 0);
  EXPECT(fsck_clean(&dev));
}

TEST(fs_journal_replays_committed_but_unwritten_metadata) {
  // Commit, then "crash" before any checkpoint: copy the device as it is
  // and mount the copy. Everything committed must be there.
  host::MemDevice dev(8192);
  ShaleFs::mkfs(&dev, {});
  ShaleFs fs;
  ASSERT(fs.mount(&dev, small_cache()) == 0);
  fs.mkdir("/logs");
  for (int i = 0; i < 20; i++) {
    std::string p = "/logs/f" + std::to_string(i);
    write_file(fs, p.c_str(), pattern(3000 + i * 100, i));
  }
  EXPECT_EQ(fs.sync(), 0);  // commit only: metadata lives in the journal
  host::MemDevice crashed(8192);
  crashed.bytes() = dev.bytes();
  ShaleFs after;
  ASSERT(after.mount(&crashed, small_cache()) == 0);
  EXPECT(after.stats().replayed_txns > 0);
  for (int i = 0; i < 20; i++) {
    std::string p = "/logs/f" + std::to_string(i);
    EXPECT(read_all(after, p.c_str()) == pattern(3000 + i * 100, i));
  }
  after.unmount();
  EXPECT(fsck_clean(&crashed));
}

TEST(fs_random_workload_matches_model_and_fscks_clean) {
  for (int journaling = 1; journaling >= 0; journaling--) {
    host::MemDevice dev(16384);
    ShaleFs::MkfsOptions mo;
    mo.journaling = journaling;
    ShaleFs::mkfs(&dev, mo);
    auto* fs = new ShaleFs;
    ASSERT(fs->mount(&dev, small_cache()) == 0);
    std::map<std::string, std::string> model;
    shale::Random rng(11 + journaling);
    for (int step = 0; step < 3000; step++) {
      std::string path = "/f" + std::to_string(rng.below(40));
      uint64_t op = rng.below(100);
      if (op < 40) {
        std::string data = pattern(rng.below(rng.chance(1, 10) ? 200000 : 9000), step);
        uint32_t ino;
        if (fs->create(path.c_str(), false, &ino) == 0) {
          uint64_t off = model.count(path) && rng.chance(1, 2) ? rng.below(model[path].size() + 1) : 0;
          std::string& m = model[path];
          if (fs->write(ino, off, data.data(), data.size()) == static_cast<int64_t>(data.size())) {
            if (m.size() < off + data.size()) m.resize(off + data.size(), '\0');
            m.replace(off, data.size(), data);
          }
        }
      } else if (op < 55) {
        if (fs->unlink(path.c_str()) == 0) model.erase(path);
      } else if (op < 70 && model.count(path)) {
        uint32_t ino;
        fs->lookup(path.c_str(), &ino);
        uint64_t len = rng.below(model[path].size() + 5000);
        if (fs->truncate(ino, len) == 0) model[path].resize(len, '\0');
      } else if (op < 80 && model.count(path)) {
        std::string to = "/f" + std::to_string(rng.below(40));
        if (fs->rename(path.c_str(), to.c_str()) == 0 && to != path) {
          model[to] = model[path];
          model.erase(path);
        }
      } else if (op < 85) {
        fs->sync();
      } else if (op < 87) {
        // remount
        EXPECT_EQ(fs->unmount(), 0);
        delete fs;
        fs = new ShaleFs;
        ASSERT(fs->mount(&dev, small_cache()) == 0);
      }
    }
    for (auto& [p, content] : model) EXPECT(read_all(*fs, p.c_str()) == content);
    EXPECT_EQ(fs->unmount(), 0);
    delete fs;
    EXPECT(fsck_clean(&dev));
  }
}
