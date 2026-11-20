#include "fs/bcache.hpp"

#include <shale/syscall.hpp>

#include "common/mem.hpp"

namespace shale::fs {
namespace {
constexpr uint32_t kChunkBuffers = 64;  // 256 KiB per data allocation (physically contiguous in the kernel)
constexpr uint32_t kNoBlock = 0xFFFFFFFFu;
}  // namespace

BufferCache::~BufferCache() {
  for (uint32_t i = 0; i < nchunks_; i++) delete[] chunks_[i];
  delete[] chunks_;
  delete[] bufs_;
  delete[] hash_;
}

int BufferCache::init(BlockDevice* dev, uint32_t buffers) {
  dev_ = dev;
  count_ = buffers < 64 ? 64 : buffers;
  bufs_ = new Buffer[count_];
  nchunks_ = (count_ + kChunkBuffers - 1) / kChunkBuffers;
  chunks_ = new uint8_t*[nchunks_];
  for (uint32_t c = 0; c < nchunks_; c++) chunks_[c] = new uint8_t[kChunkBuffers * kBlockSize];
  hash_size_ = count_ * 2;
  hash_ = new Buffer*[hash_size_];
  for (uint32_t i = 0; i < hash_size_; i++) hash_[i] = nullptr;
  for (uint32_t i = 0; i < count_; i++) {
    Buffer& b = bufs_[i];
    b = Buffer{};
    b.block = kNoBlock;
    b.data = chunks_[i / kChunkBuffers] + (i % kChunkBuffers) * kBlockSize;
    b.lru_prev = i ? &bufs_[i - 1] : nullptr;
    b.lru_next = i + 1 < count_ ? &bufs_[i + 1] : nullptr;
  }
  lru_head_ = &bufs_[0];
  lru_tail_ = &bufs_[count_ - 1];
  return 0;
}

Buffer* BufferCache::find(uint32_t block) {
  for (Buffer* b = hash_[block % hash_size_]; b; b = b->hash_next)
    if (b->block == block) return b;
  return nullptr;
}

void BufferCache::hash_insert(Buffer* b) {
  Buffer*& head = hash_[b->block % hash_size_];
  b->hash_next = head;
  head = b;
}

void BufferCache::hash_remove(Buffer* b) {
  for (Buffer** p = &hash_[b->block % hash_size_]; *p; p = &(*p)->hash_next) {
    if (*p == b) {
      *p = b->hash_next;
      b->hash_next = nullptr;
      return;
    }
  }
}

void BufferCache::touch(Buffer* b) {
  if (b == lru_head_) return;
  if (b->lru_prev) b->lru_prev->lru_next = b->lru_next;
  if (b->lru_next) b->lru_next->lru_prev = b->lru_prev;
  if (b == lru_tail_) lru_tail_ = b->lru_prev;
  b->lru_prev = nullptr;
  b->lru_next = lru_head_;
  if (lru_head_) lru_head_->lru_prev = b;
  lru_head_ = b;
  if (!lru_tail_) lru_tail_ = b;
}

int BufferCache::victim(Buffer** out) {
  for (Buffer* b = lru_tail_; b; b = b->lru_prev) {
    if (b->refs) continue;
    if (b->meta && b->txn) continue;  // part of the running transaction
    if (b->dirty) {
      int r = write_in_place(b);
      if (r < 0) return r;
    }
    if (b->block != kNoBlock) {
      hash_remove(b);
      stats_.evictions++;
    }
    b->block = kNoBlock;
    b->valid = b->meta = b->checkpoint = false;
    *out = b;
    return 0;
  }
  return -shale::sys::kENOMEM;  // the file system keeps transactions far smaller than the cache
}

int BufferCache::get(uint32_t block, Mode mode, Buffer** out) {
  Buffer* b = find(block);
  if (b) {
    stats_.hits++;
    if (mode == Mode::kFresh) {
      if (b->dirty && !b->meta) dirty_data_--;
      memset(b->data, 0, kBlockSize);
      b->valid = true;
      b->dirty = false;
      if (!b->txn) b->meta = b->checkpoint = false;
    }
  } else {
    stats_.misses++;
    int r = victim(&b);
    if (r < 0) return r;
    b->block = block;
    b->dirty = false;
    b->txn = 0;
    hash_insert(b);
    if (mode == Mode::kFresh) {
      memset(b->data, 0, kBlockSize);
      b->valid = true;
    } else {
      stats_.reads++;
      if ((r = dev_->read(block, 1, b->data)) < 0) {
        hash_remove(b);
        b->block = kNoBlock;
        return r;
      }
      b->valid = true;
    }
  }
  if (!b->valid) {  // forgotten earlier, now wanted again
    stats_.reads++;
    int r = dev_->read(block, 1, b->data);
    if (r < 0) return r;
    b->valid = true;
  }
  b->refs++;
  touch(b);
  *out = b;
  return 0;
}

void BufferCache::put(Buffer* b) { b->refs--; }

void BufferCache::note_dirty_data(Buffer* b) {
  if (!b->dirty) {
    b->dirty = true;
    dirty_data_++;
  }
}

int BufferCache::write_in_place(Buffer* b) {
  stats_.writes++;
  int r = dev_->write(b->block, 1, b->data);
  if (r < 0) return r;
  unflushed_ = true;
  if (b->dirty && !b->meta) dirty_data_--;
  b->dirty = false;
  b->checkpoint = false;
  return 0;
}

void BufferCache::forget(uint32_t block) {
  Buffer* b = find(block);
  if (!b || b->txn || b->refs) return;
  if (b->dirty && !b->meta) dirty_data_--;
  b->dirty = b->checkpoint = b->meta = false;
  hash_remove(b);
  b->block = kNoBlock;
  b->valid = false;
}

}  // namespace shale::fs
