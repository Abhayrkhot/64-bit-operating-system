#include "fs/fsck.hpp"

#include <abi/syscall.hpp>

#include "common/mem.hpp"
#include "fmt/format.hpp"
#include "fs/format.hpp"
#include "util/crc32c.hpp"

namespace os::fs {
namespace {

using namespace os::sys;
constexpr uint32_t kMetaOwner = 0xFFFFFFFFu;

struct Checker {
  BlockDevice* dev;
  FsckReport* rep;
  Superblock sb{};
  uint8_t* inode_bitmap = nullptr;
  uint8_t* block_bitmap = nullptr;
  Inode* inodes = nullptr;
  uint32_t* owner = nullptr;   // block -> owning inode
  uint32_t* refs = nullptr;    // inode -> directory entries naming it
  uint8_t* visited = nullptr;  // directories reached from the root
  uint8_t* blk = nullptr;

  ~Checker() {
    delete[] inode_bitmap;
    delete[] block_bitmap;
    delete[] reinterpret_cast<uint8_t*>(inodes);
    delete[] owner;
    delete[] refs;
    delete[] visited;
    delete[] blk;
  }

  void error(const char* fmt, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0) {
    if (rep->errors < FsckReport::kMaxMessages)
      os::fmt::format(rep->messages[rep->errors], sizeof(rep->messages[0]), fmt, a, b, c);
    rep->errors++;
  }

  static bool bit(const uint8_t* m, uint32_t i) { return m[i / 8] & (1u << (i % 8)); }

  int read_region(uint32_t start, uint32_t blocks, uint8_t* out) {
    for (uint32_t i = 0; i < blocks; i++) {
      int r = dev->read(start + i, 1, out + static_cast<uint64_t>(i) * kBlockSize);
      if (r < 0) return r;
    }
    return 0;
  }

  bool load_superblock() {
    bool found = false;
    for (uint32_t slot = 0; slot < 2; slot++) {
      if (dev->read(slot, 1, blk) < 0) continue;
      Superblock s;
      memcpy(&s, blk, sizeof(s));
      uint32_t crc = s.crc;
      s.crc = 0;
      bool ok = s.magic == kMagic && s.version == kVersion && crc == crc32c(&s, sizeof(s)) &&
                s.block_size == kBlockSize && s.total_blocks <= dev->block_count();
      s.crc = crc;
      if (ok && (!found || s.sb_seq > sb.sb_seq)) {
        sb = s;
        found = true;
      }
    }
    if (!found) return false;
    uint64_t expect_data = static_cast<uint64_t>(sb.inode_table_start) + sb.inode_table_blocks;
    if (sb.journal_start != 2 || sb.inode_bitmap_start != sb.journal_start + sb.journal_blocks ||
        sb.block_bitmap_start != sb.inode_bitmap_start + sb.inode_bitmap_blocks ||
        sb.inode_table_start != sb.block_bitmap_start + sb.block_bitmap_blocks || sb.data_start != expect_data ||
        sb.data_start >= sb.total_blocks || sb.inode_table_blocks * kInodesPerBlock != sb.inode_count) {
      error("superblock layout is inconsistent");
      return false;
    }
    return true;
  }

  // Claims `block` for inode `ino`; returns false if it can't be trusted.
  bool claim(uint32_t block, uint32_t ino) {
    if (block < sb.data_start || block >= sb.total_blocks) {
      error("inode %lu points at block %lu outside the data region", ino, block);
      return false;
    }
    if (owner[block]) {
      error("block %lu claimed by inodes %lu and %lu", block, owner[block], ino);
      return false;
    }
    owner[block] = ino;
    rep->used_blocks++;
    return true;
  }

  // Walks the block map of one inode; checks nothing is mapped past EOF.
  void check_blocks(uint32_t ino, const Inode& ip) {
    uint64_t needed = (ip.size + kBlockSize - 1) / kBlockSize;
    uint32_t counted = 0;
    auto data_block = [&](uint64_t fb, uint32_t b) {
      if (!b) return;
      if (fb >= needed) error("inode %lu maps block %lu past its size", ino, fb);
      if (claim(b, ino)) counted++;
    };
    for (uint32_t i = 0; i < kDirectBlocks; i++) data_block(i, ip.direct[i]);
    auto* ptrs = new uint32_t[kPointersPerBlock];
    if (ip.indirect && claim(ip.indirect, ino)) {
      counted++;
      if (dev->read(ip.indirect, 1, ptrs) == 0)
        for (uint32_t i = 0; i < kPointersPerBlock; i++) data_block(kDirectBlocks + i, ptrs[i]);
      if (needed <= kDirectBlocks) error("inode %lu has an indirect block it doesn't need", ino);
    }
    if (ip.dindirect && claim(ip.dindirect, ino)) {
      counted++;
      auto* outer = new uint32_t[kPointersPerBlock];
      if (dev->read(ip.dindirect, 1, outer) == 0) {
        for (uint32_t o = 0; o < kPointersPerBlock; o++) {
          if (!outer[o] || !claim(outer[o], ino)) continue;
          counted++;
          if (dev->read(outer[o], 1, ptrs) < 0) continue;
          for (uint32_t i = 0; i < kPointersPerBlock; i++)
            data_block(kDirectBlocks + kPointersPerBlock + static_cast<uint64_t>(o) * kPointersPerBlock + i,
                       ptrs[i]);
        }
      }
      delete[] outer;
    }
    delete[] ptrs;
    if (counted != ip.blocks) error("inode %lu block count %lu, found %lu", ino, ip.blocks, counted);
  }

  // Reads directory block fb of inode ip into blk; false if unmapped.
  bool dir_block(const Inode& ip, uint64_t fb) {
    uint32_t b = 0;
    if (fb < kDirectBlocks) {
      b = ip.direct[fb];
    } else if (fb < kDirectBlocks + kPointersPerBlock && ip.indirect) {
      uint32_t ptrs[kPointersPerBlock];
      if (dev->read(ip.indirect, 1, ptrs) < 0) return false;
      b = ptrs[fb - kDirectBlocks];
    }
    return b && b >= sb.data_start && b < sb.total_blocks && dev->read(b, 1, blk) == 0;
  }

  void walk_tree() {
    // Iterative DFS over directories.
    uint32_t* stack = new uint32_t[sb.inode_count];
    uint32_t top = 0;
    stack[top++] = kRootInode;
    visited[kRootInode] = 1;
    while (top) {
      uint32_t dir = stack[--top];
      const Inode& d = inodes[dir];
      if (d.size % kBlockSize) error("directory %lu has size %lu", dir, d.size);
      // Names seen in this directory, for duplicate detection (bounded).
      constexpr uint32_t kMaxNames = 4096;
      uint32_t* name_hash = new uint32_t[kMaxNames];
      uint32_t names = 0;
      for (uint64_t fb = 0; fb < d.size / kBlockSize; fb++) {
        if (!dir_block(d, fb)) continue;
        auto* ents = reinterpret_cast<const DirEnt*>(blk);
        for (uint32_t i = 0; i < kDirEntsPerBlock; i++) {
          const DirEnt& e = ents[i];
          if (!e.inode) continue;
          if (e.name_len == 0 || e.name_len > kNameMax) {
            error("directory %lu has an entry with name length %lu", dir, e.name_len);
            continue;
          }
          bool bad_char = false;
          for (uint32_t k = 0; k < e.name_len; k++)
            if (e.name[k] == '/' || e.name[k] == 0) bad_char = true;
          if (bad_char) error("directory %lu has an entry with an invalid name", dir);
          uint32_t h = crc32c(e.name, e.name_len);
          for (uint32_t k = 0; k < names; k++)
            if (name_hash[k] == h) error("directory %lu has a duplicate name (entry %lu)", dir, fb * 64 + i);
          if (names < kMaxNames) name_hash[names++] = h;
          if (e.inode >= sb.inode_count || !bit(inode_bitmap, e.inode)) {
            error("directory %lu names free or invalid inode %lu", dir, e.inode);
            continue;
          }
          const Inode& c = inodes[e.inode];
          if (static_cast<uint8_t>(c.type) != e.type) error("entry type mismatch for inode %lu", e.inode);
          refs[e.inode]++;
          if (c.type == InodeType::kDir) {
            if (visited[e.inode]) {
              error("directory %lu reachable twice", e.inode);
              continue;
            }
            if (c.parent != dir) error("directory %lu parent is %lu, found in %lu", e.inode, c.parent, dir);
            visited[e.inode] = 1;
            stack[top++] = e.inode;
          }
        }
      }
      delete[] name_hash;
    }
    delete[] stack;
  }

  int run() {
    blk = new uint8_t[kBlockSize];
    if (!load_superblock()) {
      if (!rep->errors) error("no valid superblock");
      return 0;
    }
    rep->clean_flag = sb.clean != 0;
    inode_bitmap = new uint8_t[static_cast<uint64_t>(sb.inode_bitmap_blocks) * kBlockSize];
    block_bitmap = new uint8_t[static_cast<uint64_t>(sb.block_bitmap_blocks) * kBlockSize];
    inodes = reinterpret_cast<Inode*>(new uint8_t[static_cast<uint64_t>(sb.inode_table_blocks) * kBlockSize]);
    int r;
    if ((r = read_region(sb.inode_bitmap_start, sb.inode_bitmap_blocks, inode_bitmap)) < 0 ||
        (r = read_region(sb.block_bitmap_start, sb.block_bitmap_blocks, block_bitmap)) < 0 ||
        (r = read_region(sb.inode_table_start, sb.inode_table_blocks, reinterpret_cast<uint8_t*>(inodes))) < 0)
      return r;
    owner = new uint32_t[sb.total_blocks];
    for (uint32_t b = 0; b < sb.total_blocks; b++) owner[b] = b < sb.data_start ? kMetaOwner : 0;
    refs = new uint32_t[sb.inode_count];
    visited = new uint8_t[sb.inode_count];
    memset(refs, 0, sb.inode_count * sizeof(uint32_t));
    memset(visited, 0, sb.inode_count);

    if (!bit(inode_bitmap, 0)) error("reserved inode 0 not marked in use");
    for (uint32_t ino = 1; ino < sb.inode_count; ino++) {
      const Inode& ip = inodes[ino];
      bool used = bit(inode_bitmap, ino);
      bool typed = ip.type == InodeType::kFile || ip.type == InodeType::kDir;
      if (used != typed || (!typed && ip.type != InodeType::kFree)) {
        error("inode %lu: bitmap bit %lu but type %lu", ino, used ? 1 : 0, static_cast<uint64_t>(ip.type));
        continue;
      }
      if (!used) continue;
      if (ip.type == InodeType::kDir)
        rep->dirs++;
      else {
        rep->files++;
        rep->file_bytes += ip.size;
      }
      check_blocks(ino, ip);
    }
    if (inodes[kRootInode].type != InodeType::kDir) {
      error("root inode is not a directory");
      return 0;
    }
    for (uint32_t b = sb.data_start; b < sb.total_blocks; b++) {
      bool allocated = bit(block_bitmap, b);
      if (allocated && !owner[b]) error("block %lu allocated but unreferenced", b);
      if (!allocated && owner[b]) error("block %lu used by inode %lu but free in the bitmap", b, owner[b]);
    }
    for (uint32_t b = 0; b < sb.data_start; b++)
      if (!bit(block_bitmap, b)) error("metadata block %lu free in the bitmap", b);
    walk_tree();
    for (uint32_t ino = 2; ino < sb.inode_count; ino++) {
      if (!bit(inode_bitmap, ino)) continue;
      const Inode& ip = inodes[ino];
      if (ip.type != InodeType::kFile && ip.type != InodeType::kDir) continue;
      if (refs[ino] == 0) error("inode %lu is orphaned (no directory entry)", ino);
      else if (refs[ino] != ip.links) error("inode %lu has %lu links, %lu entries", ino, ip.links, refs[ino]);
    }
    if (refs[kRootInode]) error("root directory is named by %lu entries", refs[kRootInode]);
    return 0;
  }
};

}  // namespace

int fsck(BlockDevice* dev, FsckReport* report) {
  *report = FsckReport();
  Checker c{dev, report};
  return c.run();
}

}  // namespace os::fs
