// ShaleFS: a small journaling file system (metadata journaling, ordered
// data). Portable: the kernel mounts it on virtio-blk; host tools (mkfs,
// fsck, the crash simulator) run the same code on files and recorded devices.
//
// Consistency model
//   * Every operation runs inside the running transaction, which collects
//     the metadata blocks it changes (inodes, bitmaps, directory and
//     indirect blocks). An operation reserves "credits" up front, so a
//     transaction never commits halfway through an operation.
//   * Commit (fsync, sync, size limit, timer): write dirty file data in place
//     and flush (ordered mode: data never lags the metadata that points at
//     it), then append descriptor + metadata copies + a CRC32C commit block
//     to the journal and flush. Only then is the transaction durable.
//   * Checkpoint (journal half full, sync, unmount): write committed metadata
//     in place, flush, advance the journal tail in the superblock, flush.
//   * Freed blocks are not reused until the next checkpoint, so a stale
//     journaled copy of a former metadata block can never be replayed over
//     data that now lives there.
//   * Mount replays every complete transaction after the tail.
//
// Not internally synchronized: the kernel serializes calls with a mutex.
#pragma once

#include <stdint.h>

#include "fs/bcache.hpp"
#include "fs/blockdev.hpp"
#include "fs/format.hpp"
#include "util/vec.hpp"

namespace shale::fs {

struct InodeInfo {
  InodeType type;
  uint32_t links;
  uint64_t size;
  uint32_t blocks;
  uint64_t mtime_ns;
};

class ShaleFs {
 public:
  struct MkfsOptions {
    uint32_t inode_count = 0;     // 0: one per 4 blocks
    uint32_t journal_blocks = 0;  // 0: 1/16 of the disk, 256..8192
    bool journaling = true;       // false only for the crash-test control
    uint64_t uuid = 0;            // 0: derived from the device size and a constant
  };
  struct Options {
    uint32_t cache_blocks = 1024;
    uint32_t max_txn_blocks = 256;
    uint64_t (*now_ns)() = nullptr;
    void (*log)(const char* msg) = nullptr;
    // Testing only (the crash simulator's ablation): replay a transaction
    // whenever its commit block is present, without checking its CRC.
    bool trust_commit_blocks = false;
  };
  struct Stats {
    uint64_t commits, checkpoints, flushes;
    uint64_t journal_blocks_written, inplace_meta_writes, data_writes;
    uint64_t replayed_txns, replayed_blocks;
  };

  ~ShaleFs();
  static int mkfs(BlockDevice* dev, const MkfsOptions& opts);
  int mount(BlockDevice* dev, const Options& opts);
  int unmount();  // checkpoint and mark clean
  bool mounted() const { return dev_ != nullptr; }

  int lookup(const char* path, uint32_t* ino);
  int create(const char* path, bool exclusive, uint32_t* ino);
  int mkdir(const char* path);
  int unlink(const char* path);  // files and empty directories
  int rename(const char* from, const char* to);
  int stat(uint32_t ino, InodeInfo* out);
  int64_t read(uint32_t ino, uint64_t offset, void* buf, uint64_t n);
  int64_t write(uint32_t ino, uint64_t offset, const void* buf, uint64_t n);
  int truncate(uint32_t ino, uint64_t size);
  // Fills the index-th live entry; returns 1, 0 past the end, or -errno.
  int readdir(uint32_t dir, uint64_t index, char name[58], uint32_t* child, InodeType* type);
  int fsync(uint32_t ino);
  int sync();
  int checkpoint();
  uint32_t generation(uint32_t ino);

  const Superblock& superblock() const { return sb_; }
  const Stats& stats() const { return stats_; }
  const BufferCache::Stats& cache_stats() const { return cache_.stats(); }
  uint32_t free_blocks() const { return free_blocks_; }

 private:
  // transactions
  int begin(uint32_t credits);
  int commit();
  void dirty_meta(Buffer* b);
  void dirty_data(Buffer* b);
  int recover();
  int write_superblock();
  int flush();
  uint32_t journal_phys(uint64_t pos) const;
  uint64_t journal_used() const { return head_ - tail_; }

  // allocation
  int alloc_block(uint32_t* out);
  int free_block(uint32_t block);
  int alloc_inode(uint32_t* out);
  int free_inode_bit(uint32_t ino);

  // inodes and block maps
  int get_inode(uint32_t ino, Buffer** buf, Inode** ip);
  int bmap(Inode* ip, Buffer* ib, uint64_t file_block, bool alloc, uint32_t* out, bool* fresh);
  int free_blocks_from(Inode* ip, Buffer* ib, uint64_t first_file_block);
  int release_inode(uint32_t ino);  // drop one link; free at zero

  // directories and paths
  int dir_find(uint32_t dir, const char* name, uint32_t* child, InodeType* type, uint64_t* slot);
  int dir_add(uint32_t dir, const char* name, uint32_t child, InodeType type);
  int dir_remove_slot(uint32_t dir, uint64_t slot);
  int dir_empty(uint32_t dir, bool* empty);
  int resolve(const char* path, uint32_t* ino);
  int resolve_parent(const char* path, uint32_t* parent, char* name);

  uint64_t now() const { return opts_.now_ns ? opts_.now_ns() : 0; }

  BlockDevice* dev_ = nullptr;
  Options opts_;
  Superblock sb_{};
  BufferCache cache_;
  Stats stats_{};
  bool journaling_ = true;
  Vec<Buffer*> txn_bufs_;
  uint64_t running_seq_ = 0;  // sequence number of the running transaction
  uint64_t head_ = 0, tail_ = 0;  // journal positions (monotonic block counters)
  uint8_t* deferred_ = nullptr;   // freed since the last checkpoint: not yet reusable
  uint32_t deferred_count_ = 0;
  uint32_t free_blocks_ = 0;      // clear bits in the block bitmap (including deferred)
  uint32_t block_rover_ = 0, inode_rover_ = 1;
  uint8_t* scratch_ = nullptr;    // one block for journal descriptors/commits
};

}  // namespace shale::fs
