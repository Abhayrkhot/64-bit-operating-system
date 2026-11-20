// Buffer cache: fixed set of 4 KiB buffers, hashed by block number, LRU
// eviction. Write-back: dirty buffers reach the disk on commit, checkpoint
// or eviction. Eviction never picks a pinned buffer or metadata that belongs
// to the running (uncommitted) transaction: writing that in place before its
// commit would break crash consistency.
#pragma once

#include <stdint.h>

#include "fs/blockdev.hpp"
#include "fs/format.hpp"

namespace shale::fs {

struct Buffer {
  uint32_t block;
  uint32_t refs;
  uint8_t* data;
  bool valid;       // data holds the block's contents
  bool dirty;       // newer than the disk copy
  bool meta;        // journaled metadata (otherwise ordered file data)
  bool checkpoint;  // committed to the journal, not yet written in place
  uint64_t txn;     // running transaction that modified it, 0 = none
  Buffer* hash_next;
  Buffer* lru_prev;
  Buffer* lru_next;
};

class BufferCache {
 public:
  enum class Mode { kRead, kFresh };  // kFresh: caller overwrites; contents zeroed

  struct Stats {
    uint64_t hits, misses, reads, writes, evictions;
  };

  ~BufferCache();
  int init(BlockDevice* dev, uint32_t buffers);
  // Pins and returns the buffer for `block`.
  int get(uint32_t block, Mode mode, Buffer** out);
  void put(Buffer* b);
  int write_in_place(Buffer* b);
  // Drops a freed block's cached contents (unless it's in the running txn).
  void forget(uint32_t block);
  uint32_t size() const { return count_; }
  Buffer& at(uint32_t i) { return bufs_[i]; }
  uint64_t dirty_data() const { return dirty_data_; }
  // Any write since the last flush (eviction write-back included): a commit
  // must flush before relying on it.
  bool unflushed() const { return unflushed_; }
  void flushed() { unflushed_ = false; }
  void note_dirty_data(Buffer* b);  // b just became dirty file data
  const Stats& stats() const { return stats_; }

 private:
  Buffer* find(uint32_t block);
  void hash_insert(Buffer* b);
  void hash_remove(Buffer* b);
  void touch(Buffer* b);
  int victim(Buffer** out);

  BlockDevice* dev_ = nullptr;
  Buffer* bufs_ = nullptr;
  uint8_t** chunks_ = nullptr;
  uint32_t nchunks_ = 0;
  uint32_t count_ = 0;
  Buffer** hash_ = nullptr;
  uint32_t hash_size_ = 0;
  Buffer* lru_head_ = nullptr;  // most recently used
  Buffer* lru_tail_ = nullptr;
  uint64_t dirty_data_ = 0;
  bool unflushed_ = false;
  Stats stats_{};
};

}  // namespace shale::fs
