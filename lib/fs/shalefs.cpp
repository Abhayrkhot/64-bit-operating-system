#include "fs/shalefs.hpp"

#include <shale/syscall.hpp>

#include "common/mem.hpp"
#include "util/crc32c.hpp"

namespace shale::fs {
namespace {

using namespace shale::sys;
using Mode = BufferCache::Mode;
constexpr uint32_t kBitsPerBlock = kBlockSize * 8;
constexpr uint32_t kIndirectSpan = kPointersPerBlock;

uint32_t sb_crc(const Superblock& sb) {
  Superblock c = sb;
  c.crc = 0;
  return crc32c(&c, sizeof(c));
}

bool test_bit(const uint8_t* m, uint32_t i) { return m[i / 8] & (1u << (i % 8)); }
void set_bit(uint8_t* m, uint32_t i) { m[i / 8] = static_cast<uint8_t>(m[i / 8] | (1u << (i % 8))); }
void clear_bit(uint8_t* m, uint32_t i) { m[i / 8] = static_cast<uint8_t>(m[i / 8] & ~(1u << (i % 8))); }

bool sb_valid(const Superblock& sb, uint32_t device_blocks) {
  if (sb.magic != kMagic || sb.version != kVersion || sb.block_size != kBlockSize) return false;
  if (sb.crc != sb_crc(sb)) return false;
  if (sb.total_blocks > device_blocks || sb.data_start >= sb.total_blocks || sb.journal_start != 2) return false;
  if (sb.journal_blocks < 16 || sb.journal_tail_block >= sb.journal_blocks) return false;
  if (sb.inode_count < 2 || sb.root_inode != kRootInode) return false;
  return true;
}

int name_len_ok(const char* name) {
  size_t n = strlen(name);
  if (n == 0 || n > kNameMax) return -kENAMETOOLONG;
  for (size_t i = 0; i < n; i++)
    if (name[i] == '/') return -kEINVAL;
  return 0;
}

}  // namespace

ShaleFs::~ShaleFs() {
  delete[] deferred_;
  delete[] scratch_;
}

// ---- mkfs -----------------------------------------------------------------

int ShaleFs::mkfs(BlockDevice* dev, const MkfsOptions& o) {
  uint32_t total = dev->block_count();
  if (total < 256) return -kENOSPC;
  Superblock sb{};
  sb.magic = kMagic;
  sb.version = kVersion;
  sb.block_size = kBlockSize;
  sb.uuid = o.uuid ? o.uuid : 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(total) << 20);
  sb.total_blocks = total;
  uint32_t inodes = o.inode_count ? o.inode_count : total / 4;
  inodes = shale::max<uint32_t>(64, shale::min<uint32_t>(inodes, 1u << 22));
  inodes = (inodes + kInodesPerBlock - 1) / kInodesPerBlock * kInodesPerBlock;
  uint32_t journal = o.journal_blocks ? o.journal_blocks : shale::max<uint32_t>(256, shale::min<uint32_t>(total / 16, 8192));
  if (journal > total / 4) journal = total / 4;
  sb.inode_count = inodes;
  sb.journal_start = 2;
  sb.journal_blocks = journal;
  sb.inode_bitmap_start = sb.journal_start + journal;
  sb.inode_bitmap_blocks = (inodes + kBitsPerBlock - 1) / kBitsPerBlock;
  sb.block_bitmap_start = sb.inode_bitmap_start + sb.inode_bitmap_blocks;
  sb.block_bitmap_blocks = (total + kBitsPerBlock - 1) / kBitsPerBlock;
  sb.inode_table_start = sb.block_bitmap_start + sb.block_bitmap_blocks;
  sb.inode_table_blocks = inodes / kInodesPerBlock;
  sb.data_start = sb.inode_table_start + sb.inode_table_blocks;
  if (sb.data_start + 64 > total) return -kENOSPC;
  sb.root_inode = kRootInode;
  sb.journal_tail_seq = 1;
  sb.journal_tail_block = 0;
  sb.clean = 1;
  sb.journaling = o.journaling ? 1 : 0;

  // Zero every metadata block (journal included, so no stale log survives).
  constexpr uint32_t kChunk = 16;
  auto* zeros = new uint8_t[kChunk * kBlockSize];
  memset(zeros, 0, kChunk * kBlockSize);
  int r = 0;
  for (uint32_t b = 0; b < sb.data_start && r == 0; b += kChunk)
    r = dev->write(b, shale::min(kChunk, sb.data_start - b), zeros);
  auto* blk = new uint8_t[kBlockSize];
  if (r == 0) {  // inode bitmap: inode 0 reserved, 1 = root
    memset(blk, 0, kBlockSize);
    set_bit(blk, 0);
    set_bit(blk, kRootInode);
    r = dev->write(sb.inode_bitmap_start, 1, blk);
  }
  for (uint32_t i = 0; i < sb.block_bitmap_blocks && r == 0; i++) {  // metadata blocks are in use
    memset(blk, 0, kBlockSize);
    for (uint32_t bit = 0; bit < kBitsPerBlock; bit++) {
      uint64_t block = static_cast<uint64_t>(i) * kBitsPerBlock + bit;
      if (block < sb.data_start || block >= total) set_bit(blk, bit);
    }
    r = dev->write(sb.block_bitmap_start + i, 1, blk);
  }
  if (r == 0) {
    memset(blk, 0, kBlockSize);
    auto* root = reinterpret_cast<Inode*>(blk) + kRootInode;
    root->type = InodeType::kDir;
    root->links = 1;
    root->parent = kRootInode;
    root->generation = 1;
    r = dev->write(sb.inode_table_start, 1, blk);
  }
  for (uint64_t slot = 0; slot < 2 && r == 0; slot++) {
    Superblock s = sb;
    s.sb_seq = slot == 0 ? 2 : 1;  // slot 0 is the newer copy
    s.crc = sb_crc(s);
    memset(blk, 0, kBlockSize);
    memcpy(blk, &s, sizeof(s));
    r = dev->write(static_cast<uint32_t>(slot), 1, blk);
  }
  if (r == 0) r = dev->flush();
  delete[] zeros;
  delete[] blk;
  return r;
}

// ---- mount / unmount --------------------------------------------------------

int ShaleFs::mount(BlockDevice* dev, const Options& opts) {
  if (dev_) return -kEBUSY;
  opts_ = opts;
  auto* blk = new uint8_t[kBlockSize];
  Superblock best{};
  bool found = false;
  for (uint32_t slot = 0; slot < 2; slot++) {
    if (dev->read(slot, 1, blk) < 0) continue;
    Superblock s;
    memcpy(&s, blk, sizeof(s));
    if (sb_valid(s, dev->block_count()) && (!found || s.sb_seq > best.sb_seq)) {
      best = s;
      found = true;
    }
  }
  delete[] blk;
  if (!found) return -kEINVAL;
  sb_ = best;
  dev_ = dev;
  journaling_ = sb_.journaling != 0;
  if (opts_.max_txn_blocks > sb_.journal_blocks / 4) opts_.max_txn_blocks = sb_.journal_blocks / 4;
  scratch_ = new uint8_t[kBlockSize];
  deferred_ = new uint8_t[(sb_.total_blocks + 7) / 8];
  memset(deferred_, 0, (sb_.total_blocks + 7) / 8);
  deferred_count_ = 0;
  cache_.init(dev, opts_.cache_blocks);
  int r = journaling_ ? recover() : 0;
  if (r == 0) {
    free_blocks_ = 0;
    for (uint32_t i = 0; i < sb_.block_bitmap_blocks && r == 0; i++) {
      Buffer* b;
      if ((r = cache_.get(sb_.block_bitmap_start + i, Mode::kRead, &b)) < 0) break;
      for (uint32_t bit = 0; bit < kBitsPerBlock; bit++) {
        uint64_t block = static_cast<uint64_t>(i) * kBitsPerBlock + bit;
        if (block < sb_.total_blocks && !test_bit(b->data, bit)) free_blocks_++;
      }
      cache_.put(b);
    }
  }
  if (r == 0) {
    block_rover_ = sb_.data_start;
    sb_.clean = 0;
    r = write_superblock();
    if (r == 0) r = flush();
  }
  if (r < 0) dev_ = nullptr;
  return r;
}

int ShaleFs::unmount() {
  if (!dev_) return -kEINVAL;
  int r = checkpoint();
  if (r == 0) {
    sb_.clean = 1;
    r = write_superblock();
    if (r == 0) r = flush();
  }
  dev_ = nullptr;
  return r;
}

int ShaleFs::write_superblock() {
  sb_.sb_seq++;
  sb_.crc = sb_crc(sb_);
  memset(scratch_, 0, kBlockSize);
  memcpy(scratch_, &sb_, sizeof(sb_));
  return dev_->write(static_cast<uint32_t>(sb_.sb_seq & 1), 1, scratch_);
}

int ShaleFs::flush() {
  stats_.flushes++;
  int r = dev_->flush();
  if (r == 0) cache_.flushed();
  return r;
}

uint32_t ShaleFs::journal_phys(uint64_t pos) const {
  return sb_.journal_start + static_cast<uint32_t>(pos % sb_.journal_blocks);
}

// ---- recovery -----------------------------------------------------------------

int ShaleFs::recover() {
  const uint32_t J = sb_.journal_blocks;
  uint64_t pos = sb_.journal_tail_block;
  uint64_t seq = sb_.journal_tail_seq;
  Vec<uint32_t> targets;
  Vec<uint8_t*> copies;
  auto drop_copies = [&]() {
    for (uint8_t* c : copies) delete[] c;
    copies.clear();
    targets.clear();
  };
  uint64_t consumed = 0;
  int r = 0;
  for (;;) {
    drop_copies();
    uint64_t p = pos;
    uint32_t crc = 0, nblocks = 0;
    bool committed = false;
    while (consumed + nblocks + 1 <= J) {
      if ((r = dev_->read(journal_phys(p), 1, scratch_)) < 0) break;
      JournalHeader h;
      memcpy(&h, scratch_, sizeof(h));
      if (h.magic != kJournalMagic || h.uuid != sb_.uuid || h.seq != seq) break;
      if (h.type == JournalBlockType::kCommit) {
        JournalCommit c;
        memcpy(&c, scratch_, sizeof(c));
        uint32_t self = c.crc;
        c.crc = 0;
        committed = self == crc32c(&c, sizeof(c)) && c.total_blocks == nblocks && nblocks > 0 &&
                    (c.data_crc == crc || opts_.trust_commit_blocks);
        p++;
        break;
      }
      if (h.type != JournalBlockType::kDescriptor) break;
      JournalDescriptor d;
      memcpy(&d, scratch_, sizeof(d));
      if (d.count == 0 || d.count > kTargetsPerDescriptor || consumed + nblocks + 1 + d.count + 1 > J) break;
      crc = crc32c(crc, scratch_, kBlockSize);
      p++;
      nblocks++;
      bool bad = false;
      for (uint32_t i = 0; i < d.count && !bad; i++) {
        uint32_t t = d.targets[i];
        bool in_journal = t >= sb_.journal_start && t < sb_.journal_start + J;
        if (t < 2 || t >= sb_.total_blocks || in_journal) {
          bad = true;
          break;
        }
        auto* copy = new uint8_t[kBlockSize];
        if ((r = dev_->read(journal_phys(p), 1, copy)) < 0) {
          delete[] copy;
          bad = true;
          break;
        }
        crc = crc32c(crc, copy, kBlockSize);
        copies.push(copy);
        targets.push(t);
        p++;
        nblocks++;
      }
      if (bad) break;
    }
    if (r < 0 || !committed) break;
    for (size_t i = 0; i < targets.size() && r == 0; i++) r = dev_->write(targets[i], 1, copies[i]);
    if (r < 0) break;
    stats_.replayed_txns++;
    stats_.replayed_blocks += targets.size();
    consumed += nblocks + 1;
    pos = p % J;
    seq++;
  }
  drop_copies();
  if (r < 0) return r;
  if ((r = flush()) < 0) return r;
  // The journal is now logically empty. Skip one sequence number so blocks
  // of a transaction that failed its checks can never look current again.
  head_ = tail_ = pos;
  running_seq_ = seq + 1;
  sb_.journal_tail_block = static_cast<uint32_t>(pos);
  sb_.journal_tail_seq = running_seq_;
  if (stats_.replayed_txns && opts_.log) opts_.log("shalefs: journal replayed");
  return 0;
}

// ---- transactions -----------------------------------------------------------------

void ShaleFs::dirty_meta(Buffer* b) {
  b->meta = true;
  b->dirty = true;
  if (journaling_ && b->txn != running_seq_) {
    b->txn = running_seq_;
    txn_bufs_.push(b);
  }
}

void ShaleFs::dirty_data(Buffer* b) { cache_.note_dirty_data(b); }

int ShaleFs::begin(uint32_t credits) {
  int r;
  credits += sb_.block_bitmap_blocks + sb_.inode_bitmap_blocks;
  // Frees wait for a checkpoint before reuse; take one when that's what
  // stands between this operation and free space.
  if (deferred_count_ && free_blocks_ < deferred_count_ + credits + 64) {
    if ((r = checkpoint()) < 0) return r;
  }
  if (!journaling_) return 0;
  if (txn_bufs_.size() + credits > opts_.max_txn_blocks) {
    if ((r = commit()) < 0) return r;
  }
  uint64_t worst = opts_.max_txn_blocks + opts_.max_txn_blocks / kTargetsPerDescriptor + 2;
  if (journal_used() + worst > sb_.journal_blocks) {
    if ((r = checkpoint()) < 0) return r;
  }
  return 0;
}

int ShaleFs::commit() {
  int r;
  if (!journaling_) {
    // Control mode (no journal): write everything in place, then flush.
    bool any = false;
    for (uint32_t i = 0; i < cache_.size(); i++) {
      Buffer& b = cache_.at(i);
      if (!b.dirty) continue;
      if (b.meta)
        stats_.inplace_meta_writes++;
      else
        stats_.data_writes++;
      if ((r = cache_.write_in_place(&b)) < 0) return r;
      any = true;
    }
    if (any || cache_.unflushed()) {
      stats_.commits++;
      return flush();
    }
    return 0;
  }
  if (txn_bufs_.empty() && cache_.dirty_data() == 0 && !cache_.unflushed()) return 0;

  // 1. Ordered data first, durable before any metadata that refers to it.
  // Data written back early by eviction counts too: flush it before the
  // journal blocks are issued, or the device could persist the commit first.
  if (cache_.dirty_data() || cache_.unflushed()) {
    for (uint32_t i = 0; i < cache_.size(); i++) {
      Buffer& b = cache_.at(i);
      if (b.dirty && !b.meta) {
        stats_.data_writes++;
        if ((r = cache_.write_in_place(&b)) < 0) return r;
      }
    }
    if ((r = flush()) < 0) return r;
  }
  if (txn_bufs_.empty()) return 0;

  // 2. Descriptor groups + metadata copies + commit block, one flush.
  const uint32_t n = static_cast<uint32_t>(txn_bufs_.size());
  const uint32_t groups = (n + kTargetsPerDescriptor - 1) / kTargetsPerDescriptor;
  const uint32_t total = n + groups + 1;
  if (journal_used() + total > sb_.journal_blocks) return -kENOSPC;
  uint64_t pos = head_;
  uint32_t crc = 0;
  for (uint32_t g = 0; g < groups; g++) {
    memset(scratch_, 0, kBlockSize);
    auto* d = reinterpret_cast<JournalDescriptor*>(scratch_);
    d->h = {kJournalMagic, JournalBlockType::kDescriptor, running_seq_, sb_.uuid};
    uint32_t first = g * kTargetsPerDescriptor;
    d->count = shale::min(kTargetsPerDescriptor, n - first);
    for (uint32_t i = 0; i < d->count; i++) d->targets[i] = txn_bufs_[first + i]->block;
    crc = crc32c(crc, scratch_, kBlockSize);
    if ((r = dev_->write(journal_phys(pos++), 1, scratch_)) < 0) return r;
    for (uint32_t i = 0; i < d->count; i++) {
      Buffer* b = txn_bufs_[first + i];
      crc = crc32c(crc, b->data, kBlockSize);
      if ((r = dev_->write(journal_phys(pos++), 1, b->data)) < 0) return r;
    }
  }
  memset(scratch_, 0, kBlockSize);
  auto* c = reinterpret_cast<JournalCommit*>(scratch_);
  c->h = {kJournalMagic, JournalBlockType::kCommit, running_seq_, sb_.uuid};
  c->total_blocks = n + groups;
  c->data_crc = crc;
  c->crc = 0;
  c->crc = crc32c(c, sizeof(*c));
  if ((r = dev_->write(journal_phys(pos++), 1, scratch_)) < 0) return r;
  if ((r = flush()) < 0) return r;

  // 3. Durable. The metadata now only needs writing in place eventually.
  for (Buffer* b : txn_bufs_) {
    b->txn = 0;
    b->checkpoint = true;
  }
  txn_bufs_.clear();
  head_ = pos;
  running_seq_++;
  stats_.commits++;
  stats_.journal_blocks_written += total;
  if (journal_used() > sb_.journal_blocks / 2) return checkpoint();
  return 0;
}

int ShaleFs::checkpoint() {
  int r = commit();
  if (r < 0) return r;
  if (!journaling_) {
    memset(deferred_, 0, (sb_.total_blocks + 7) / 8);
    deferred_count_ = 0;
    return 0;
  }
  bool wrote = false;
  for (uint32_t i = 0; i < cache_.size(); i++) {
    Buffer& b = cache_.at(i);
    if (b.checkpoint && !b.txn) {
      stats_.inplace_meta_writes++;
      if ((r = cache_.write_in_place(&b)) < 0) return r;
      wrote = true;
    }
  }
  if (head_ == tail_ && !wrote && deferred_count_ == 0) return 0;
  if ((r = flush()) < 0) return r;  // in-place copies durable before the tail moves
  tail_ = head_;
  sb_.journal_tail_block = static_cast<uint32_t>(tail_ % sb_.journal_blocks);
  sb_.journal_tail_seq = running_seq_;
  if ((r = write_superblock()) < 0) return r;
  if ((r = flush()) < 0) return r;
  memset(deferred_, 0, (sb_.total_blocks + 7) / 8);
  deferred_count_ = 0;
  stats_.checkpoints++;
  return 0;
}

int ShaleFs::fsync(uint32_t) { return commit(); }
int ShaleFs::sync() { return commit(); }

// ---- allocation -------------------------------------------------------------------

int ShaleFs::alloc_block(uint32_t* out) {
  const uint32_t first = sb_.data_start, n = sb_.total_blocks - sb_.data_start;
  Buffer* bm = nullptr;
  uint32_t bm_index = ~0u;
  int r = 0;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t b = first + (block_rover_ - first + i) % n;
    uint32_t idx = b / kBitsPerBlock, bit = b % kBitsPerBlock;
    if (idx != bm_index) {
      if (bm) cache_.put(bm);
      if ((r = cache_.get(sb_.block_bitmap_start + idx, Mode::kRead, &bm)) < 0) return r;
      bm_index = idx;
    }
    if (bm->data[bit / 8] == 0xFF && bit % 8 == 0 && i + 8 <= n && b + 8 <= sb_.total_blocks) {
      i += 7;  // whole byte in use
      continue;
    }
    if (test_bit(bm->data, bit) || test_bit(deferred_, b)) continue;
    set_bit(bm->data, bit);
    dirty_meta(bm);
    cache_.put(bm);
    free_blocks_--;
    block_rover_ = b + 1 < sb_.total_blocks ? b + 1 : first;
    *out = b;
    return 0;
  }
  if (bm) cache_.put(bm);
  return -kENOSPC;
}

int ShaleFs::free_block(uint32_t block) {
  if (block < sb_.data_start || block >= sb_.total_blocks) return -kEIO;
  Buffer* bm;
  int r = cache_.get(sb_.block_bitmap_start + block / kBitsPerBlock, Mode::kRead, &bm);
  if (r < 0) return r;
  if (!test_bit(bm->data, block % kBitsPerBlock)) {
    cache_.put(bm);
    return -kEIO;  // double free: on-disk corruption
  }
  clear_bit(bm->data, block % kBitsPerBlock);
  dirty_meta(bm);
  cache_.put(bm);
  free_blocks_++;
  if (journaling_) {
    set_bit(deferred_, block);
    deferred_count_++;
  }
  cache_.forget(block);
  return 0;
}

int ShaleFs::alloc_inode(uint32_t* out) {
  for (uint32_t i = 0; i < sb_.inode_count; i++) {
    uint32_t ino = 1 + (inode_rover_ - 1 + i) % (sb_.inode_count - 1);
    Buffer* bm;
    int r = cache_.get(sb_.inode_bitmap_start + ino / kBitsPerBlock, Mode::kRead, &bm);
    if (r < 0) return r;
    if (!test_bit(bm->data, ino % kBitsPerBlock)) {
      set_bit(bm->data, ino % kBitsPerBlock);
      dirty_meta(bm);
      cache_.put(bm);
      inode_rover_ = ino + 1;
      *out = ino;
      return 0;
    }
    cache_.put(bm);
  }
  return -kENOSPC;
}

int ShaleFs::free_inode_bit(uint32_t ino) {
  Buffer* bm;
  int r = cache_.get(sb_.inode_bitmap_start + ino / kBitsPerBlock, Mode::kRead, &bm);
  if (r < 0) return r;
  clear_bit(bm->data, ino % kBitsPerBlock);
  dirty_meta(bm);
  cache_.put(bm);
  return 0;
}

// ---- inodes and block maps --------------------------------------------------------

int ShaleFs::get_inode(uint32_t ino, Buffer** buf, Inode** ip) {
  if (ino == 0 || ino >= sb_.inode_count) return -kENOENT;
  int r = cache_.get(sb_.inode_table_start + ino / kInodesPerBlock, Mode::kRead, buf);
  if (r < 0) return r;
  *ip = reinterpret_cast<Inode*>((*buf)->data) + ino % kInodesPerBlock;
  return 0;
}

uint32_t ShaleFs::generation(uint32_t ino) {
  Buffer* b;
  Inode* ip;
  if (get_inode(ino, &b, &ip) < 0) return 0;
  uint32_t g = ip->type == InodeType::kFree ? 0 : ip->generation;
  cache_.put(b);
  return g;
}

// Resolves a file block to a disk block, optionally allocating it (and any
// missing indirect blocks). *fresh is set when a new data block was allocated.
int ShaleFs::bmap(Inode* ip, Buffer* ib, uint64_t fb, bool alloc, uint32_t* out, bool* fresh) {
  *fresh = false;
  *out = 0;
  if (fb >= kMaxFileBlocks) return -kEFBIG;
  int r;
  auto new_block = [&](uint32_t* slot, Buffer* holder, bool meta_block) -> int {
    uint32_t nb;
    int rr = alloc_block(&nb);
    if (rr < 0) return rr;
    *slot = nb;
    ip->blocks++;
    dirty_meta(holder);
    if (holder != ib) dirty_meta(ib);
    if (meta_block) {  // indirect blocks start zeroed, and are journaled
      Buffer* nbuf;
      if ((rr = cache_.get(nb, Mode::kFresh, &nbuf)) < 0) return rr;
      dirty_meta(nbuf);
      cache_.put(nbuf);
    }
    return 0;
  };
  if (fb < kDirectBlocks) {
    if (!ip->direct[fb] && alloc) {
      if ((r = new_block(&ip->direct[fb], ib, false)) < 0) return r;
      *fresh = true;
    }
    *out = ip->direct[fb];
    return 0;
  }
  fb -= kDirectBlocks;
  uint32_t* top_slot;
  int levels;
  if (fb < kIndirectSpan) {
    top_slot = &ip->indirect;
    levels = 1;
  } else {
    fb -= kIndirectSpan;
    top_slot = &ip->dindirect;
    levels = 2;
  }
  if (!*top_slot) {
    if (!alloc) return 0;
    if ((r = new_block(top_slot, ib, true)) < 0) return r;
  }
  Buffer* level_buf;
  if ((r = cache_.get(*top_slot, Mode::kRead, &level_buf)) < 0) return r;
  if (levels == 2) {
    auto* ptrs = reinterpret_cast<uint32_t*>(level_buf->data);
    uint32_t* mid_slot = &ptrs[fb / kIndirectSpan];
    if (!*mid_slot) {
      if (!alloc) {
        cache_.put(level_buf);
        return 0;
      }
      if ((r = new_block(mid_slot, level_buf, true)) < 0) {
        cache_.put(level_buf);
        return r;
      }
    }
    Buffer* mid;
    r = cache_.get(*mid_slot, Mode::kRead, &mid);
    cache_.put(level_buf);
    if (r < 0) return r;
    level_buf = mid;
    fb %= kIndirectSpan;
  }
  auto* ptrs = reinterpret_cast<uint32_t*>(level_buf->data);
  if (!ptrs[fb] && alloc) {
    if ((r = new_block(&ptrs[fb], level_buf, false)) < 0) {
      cache_.put(level_buf);
      return r;
    }
    *fresh = true;
  }
  *out = ptrs[fb];
  cache_.put(level_buf);
  return 0;
}

// Frees every block at file index >= first. When the whole file goes
// (first == 0) the pointer blocks are freed without being rewritten.
int ShaleFs::free_blocks_from(Inode* ip, Buffer* ib, uint64_t first) {
  int r;
  const bool whole = first == 0;
  for (uint64_t fb = first; fb < kDirectBlocks; fb++) {
    if (!ip->direct[fb]) continue;
    if ((r = free_block(ip->direct[fb])) < 0) return r;
    ip->direct[fb] = 0;
    ip->blocks--;
  }
  // Frees entries [from, span) of a pointer block; *emptied when from == 0.
  auto free_ptr_block = [&](uint32_t block, uint64_t from, int depth, auto& self) -> int {
    Buffer* b;
    int rr = cache_.get(block, Mode::kRead, &b);
    if (rr < 0) return rr;
    auto* ptrs = reinterpret_cast<uint32_t*>(b->data);
    uint64_t child_span = depth == 2 ? kIndirectSpan : 1;
    bool changed = false;
    for (uint64_t i = from / child_span; i < kIndirectSpan && rr == 0; i++) {
      if (!ptrs[i]) continue;
      uint64_t child_from = (i == from / child_span) ? from % child_span : 0;
      if (depth == 2) {
        rr = self(ptrs[i], child_from, 1, self);
        if (rr < 0) break;
        if (child_from != 0) continue;  // partially kept
      }
      if ((rr = free_block(ptrs[i])) < 0) break;
      ip->blocks--;
      if (!whole) {
        ptrs[i] = 0;
        changed = true;
      }
    }
    if (changed && from != 0) dirty_meta(b);
    cache_.put(b);
    return rr;
  };
  uint64_t f1 = first > kDirectBlocks ? first - kDirectBlocks : 0;
  if (ip->indirect && f1 < kIndirectSpan) {
    if ((r = free_ptr_block(ip->indirect, f1, 1, free_ptr_block)) < 0) return r;
    if (f1 == 0) {
      if ((r = free_block(ip->indirect)) < 0) return r;
      ip->indirect = 0;
      ip->blocks--;
    }
  }
  uint64_t f2 = first > kDirectBlocks + kIndirectSpan ? first - kDirectBlocks - kIndirectSpan : 0;
  if (ip->dindirect) {
    if ((r = free_ptr_block(ip->dindirect, f2, 2, free_ptr_block)) < 0) return r;
    if (f2 == 0) {
      if ((r = free_block(ip->dindirect)) < 0) return r;
      ip->dindirect = 0;
      ip->blocks--;
    }
  }
  dirty_meta(ib);
  return 0;
}

int ShaleFs::release_inode(uint32_t ino) {
  Buffer* ib;
  Inode* ip;
  int r = get_inode(ino, &ib, &ip);
  if (r < 0) return r;
  if (ip->links > 1) {
    ip->links--;
    dirty_meta(ib);
    cache_.put(ib);
    return 0;
  }
  r = free_blocks_from(ip, ib, 0);
  if (r == 0) {
    uint32_t gen = ip->generation;
    memset(ip, 0, sizeof(*ip));
    ip->generation = gen;  // kept so the next user of this inode gets a new one
    dirty_meta(ib);
    r = free_inode_bit(ino);
  }
  cache_.put(ib);
  return r;
}

// ---- directories -----------------------------------------------------------------

int ShaleFs::dir_find(uint32_t dir, const char* name, uint32_t* child, InodeType* type, uint64_t* slot) {
  Buffer* ib;
  Inode* ip;
  int r = get_inode(dir, &ib, &ip);
  if (r < 0) return r;
  if (ip->type != InodeType::kDir) {
    cache_.put(ib);
    return -kENOTDIR;
  }
  size_t len = strlen(name);
  uint64_t nblocks = ip->size / kBlockSize;
  r = -kENOENT;
  for (uint64_t fb = 0; fb < nblocks && r == -kENOENT; fb++) {
    uint32_t block;
    bool fresh;
    int rr = bmap(ip, ib, fb, false, &block, &fresh);
    if (rr < 0) {
      r = rr;
      break;
    }
    if (!block) continue;
    Buffer* db;
    if ((rr = cache_.get(block, Mode::kRead, &db)) < 0) {
      r = rr;
      break;
    }
    auto* ents = reinterpret_cast<const DirEnt*>(db->data);
    for (uint32_t i = 0; i < kDirEntsPerBlock; i++) {
      if (ents[i].inode && ents[i].name_len == len && memcmp(ents[i].name, name, len) == 0) {
        *child = ents[i].inode;
        if (type) *type = static_cast<InodeType>(ents[i].type);
        if (slot) *slot = fb * kDirEntsPerBlock + i;
        r = 0;
        break;
      }
    }
    cache_.put(db);
  }
  cache_.put(ib);
  return r;
}

int ShaleFs::dir_add(uint32_t dir, const char* name, uint32_t child, InodeType type) {
  Buffer* ib;
  Inode* ip;
  int r = get_inode(dir, &ib, &ip);
  if (r < 0) return r;
  uint64_t nblocks = ip->size / kBlockSize;
  auto fill = [&](DirEnt* e) {
    memset(e, 0, sizeof(*e));
    e->inode = child;
    e->type = static_cast<uint8_t>(type);
    e->name_len = static_cast<uint8_t>(strlen(name));
    memcpy(e->name, name, e->name_len);
  };
  for (uint64_t fb = 0; fb < nblocks; fb++) {
    uint32_t block;
    bool fresh;
    if ((r = bmap(ip, ib, fb, false, &block, &fresh)) < 0) break;
    if (!block) continue;
    Buffer* db;
    if ((r = cache_.get(block, Mode::kRead, &db)) < 0) break;
    auto* ents = reinterpret_cast<DirEnt*>(db->data);
    for (uint32_t i = 0; i < kDirEntsPerBlock; i++) {
      if (!ents[i].inode) {
        fill(&ents[i]);
        dirty_meta(db);
        cache_.put(db);
        ip->mtime_ns = now();
        dirty_meta(ib);
        cache_.put(ib);
        return 0;
      }
    }
    cache_.put(db);
  }
  if (r < 0) {
    cache_.put(ib);
    return r;
  }
  // No free slot: append a block.
  uint32_t block;
  bool fresh;
  if ((r = bmap(ip, ib, nblocks, true, &block, &fresh)) < 0) {
    cache_.put(ib);
    return r;
  }
  Buffer* db;
  if ((r = cache_.get(block, Mode::kFresh, &db)) == 0) {
    fill(reinterpret_cast<DirEnt*>(db->data));
    dirty_meta(db);
    cache_.put(db);
    ip->size += kBlockSize;
    ip->mtime_ns = now();
    dirty_meta(ib);
  }
  cache_.put(ib);
  return r;
}

int ShaleFs::dir_remove_slot(uint32_t dir, uint64_t slot) {
  Buffer* ib;
  Inode* ip;
  int r = get_inode(dir, &ib, &ip);
  if (r < 0) return r;
  uint32_t block;
  bool fresh;
  if ((r = bmap(ip, ib, slot / kDirEntsPerBlock, false, &block, &fresh)) == 0 && block) {
    Buffer* db;
    if ((r = cache_.get(block, Mode::kRead, &db)) == 0) {
      memset(reinterpret_cast<DirEnt*>(db->data) + slot % kDirEntsPerBlock, 0, sizeof(DirEnt));
      dirty_meta(db);
      cache_.put(db);
      ip->mtime_ns = now();
      dirty_meta(ib);
    }
  }
  cache_.put(ib);
  return r;
}

int ShaleFs::dir_empty(uint32_t dir, bool* empty) {
  char name[58];
  uint32_t child;
  InodeType type;
  int r = readdir(dir, 0, name, &child, &type);
  if (r < 0) return r;
  *empty = r == 0;
  return 0;
}

int ShaleFs::readdir(uint32_t dir, uint64_t index, char name[58], uint32_t* child, InodeType* type) {
  Buffer* ib;
  Inode* ip;
  int r = get_inode(dir, &ib, &ip);
  if (r < 0) return r;
  if (ip->type != InodeType::kDir) {
    cache_.put(ib);
    return -kENOTDIR;
  }
  uint64_t seen = 0, nblocks = ip->size / kBlockSize;
  r = 0;
  for (uint64_t fb = 0; fb < nblocks && r == 0; fb++) {
    uint32_t block;
    bool fresh;
    int rr = bmap(ip, ib, fb, false, &block, &fresh);
    if (rr < 0) {
      r = rr;
      break;
    }
    if (!block) continue;
    Buffer* db;
    if ((rr = cache_.get(block, Mode::kRead, &db)) < 0) {
      r = rr;
      break;
    }
    auto* ents = reinterpret_cast<const DirEnt*>(db->data);
    for (uint32_t i = 0; i < kDirEntsPerBlock; i++) {
      if (!ents[i].inode) continue;
      if (seen++ == index) {
        memcpy(name, ents[i].name, ents[i].name_len);
        name[ents[i].name_len] = 0;
        *child = ents[i].inode;
        *type = static_cast<InodeType>(ents[i].type);
        r = 1;
        break;
      }
    }
    cache_.put(db);
  }
  cache_.put(ib);
  return r;
}

// ---- paths -----------------------------------------------------------------------

int ShaleFs::resolve(const char* path, uint32_t* ino) {
  if (path[0] != '/') return -kENOENT;
  uint32_t cur = kRootInode;
  const char* p = path;
  char name[kNameMax + 1];
  while (*p) {
    while (*p == '/') p++;
    if (!*p) break;
    const char* start = p;
    while (*p && *p != '/') p++;
    size_t len = static_cast<size_t>(p - start);
    if (len > kNameMax) return -kENAMETOOLONG;
    memcpy(name, start, len);
    name[len] = 0;
    if (!strcmp(name, ".")) continue;
    if (!strcmp(name, "..")) {
      Buffer* b;
      Inode* ip;
      int r = get_inode(cur, &b, &ip);
      if (r < 0) return r;
      cur = ip->parent ? ip->parent : kRootInode;
      cache_.put(b);
      continue;
    }
    uint32_t child;
    int r = dir_find(cur, name, &child, nullptr, nullptr);
    if (r < 0) return r;
    cur = child;
  }
  *ino = cur;
  return 0;
}

int ShaleFs::resolve_parent(const char* path, uint32_t* parent, char* name) {
  size_t len = strlen(path);
  while (len > 1 && path[len - 1] == '/') len--;
  if (path[0] != '/' || len <= 1) return -kEINVAL;  // "/" itself has no parent entry
  size_t slash = len;
  while (slash > 0 && path[slash - 1] != '/') slash--;
  size_t nlen = len - slash;
  if (nlen > kNameMax) return -kENAMETOOLONG;
  memcpy(name, path + slash, nlen);
  name[nlen] = 0;
  if (!strcmp(name, ".") || !strcmp(name, "..")) return -kEINVAL;
  int r = name_len_ok(name);
  if (r < 0) return r;
  char dir[256];
  if (slash >= sizeof(dir)) return -kENAMETOOLONG;
  memcpy(dir, path, slash);
  dir[slash] = 0;
  if (slash == 0) {
    dir[0] = '/';
    dir[1] = 0;
  }
  return resolve(dir, parent);
}

// ---- operations ------------------------------------------------------------------

int ShaleFs::lookup(const char* path, uint32_t* ino) { return resolve(path, ino); }

namespace {
constexpr uint32_t kOpCredits = 16;
}

int ShaleFs::create(const char* path, bool exclusive, uint32_t* ino) {
  uint32_t parent, child;
  char name[kNameMax + 1];
  int r = resolve_parent(path, &parent, name);
  if (r < 0) return r;
  InodeType type;
  r = dir_find(parent, name, &child, &type, nullptr);
  if (r == 0) {
    if (exclusive) return -kEEXIST;
    if (type == InodeType::kDir) return -kEISDIR;
    *ino = child;
    return 0;
  }
  if (r != -kENOENT) return r;
  if ((r = begin(kOpCredits)) < 0) return r;
  uint32_t n;
  if ((r = alloc_inode(&n)) < 0) return r;
  Buffer* ib;
  Inode* ip;
  if ((r = get_inode(n, &ib, &ip)) < 0) return r;
  uint32_t gen = ip->generation + 1;
  memset(ip, 0, sizeof(*ip));
  ip->type = InodeType::kFile;
  ip->links = 1;
  ip->parent = parent;
  ip->generation = gen;
  ip->mtime_ns = ip->ctime_ns = now();
  dirty_meta(ib);
  cache_.put(ib);
  if ((r = dir_add(parent, name, n, InodeType::kFile)) < 0) {
    release_inode(n);  // same transaction: undo cleanly
    return r;
  }
  *ino = n;
  return 0;
}

int ShaleFs::mkdir(const char* path) {
  uint32_t parent, child;
  char name[kNameMax + 1];
  int r = resolve_parent(path, &parent, name);
  if (r < 0) return r;
  if ((r = dir_find(parent, name, &child, nullptr, nullptr)) == 0) return -kEEXIST;
  if (r != -kENOENT) return r;
  if ((r = begin(kOpCredits)) < 0) return r;
  uint32_t n;
  if ((r = alloc_inode(&n)) < 0) return r;
  Buffer* ib;
  Inode* ip;
  if ((r = get_inode(n, &ib, &ip)) < 0) return r;
  uint32_t gen = ip->generation + 1;
  memset(ip, 0, sizeof(*ip));
  ip->type = InodeType::kDir;
  ip->links = 1;
  ip->parent = parent;
  ip->generation = gen;
  ip->mtime_ns = ip->ctime_ns = now();
  dirty_meta(ib);
  cache_.put(ib);
  if ((r = dir_add(parent, name, n, InodeType::kDir)) < 0) {
    release_inode(n);
    return r;
  }
  return 0;
}

int ShaleFs::unlink(const char* path) {
  uint32_t parent, child;
  char name[kNameMax + 1];
  int r = resolve_parent(path, &parent, name);
  if (r < 0) return r;
  InodeType type;
  uint64_t slot;
  if ((r = dir_find(parent, name, &child, &type, &slot)) < 0) return r;
  if (type == InodeType::kDir) {
    bool empty;
    if ((r = dir_empty(child, &empty)) < 0) return r;
    if (!empty) return -kENOTEMPTY;
  }
  if ((r = begin(kOpCredits)) < 0) return r;
  if ((r = dir_remove_slot(parent, slot)) < 0) return r;
  return release_inode(child);
}

int ShaleFs::rename(const char* from, const char* to) {
  uint32_t sp, dp, src, dst;
  char sname[kNameMax + 1], dname[kNameMax + 1];
  int r = resolve_parent(from, &sp, sname);
  if (r < 0) return r;
  if ((r = resolve_parent(to, &dp, dname)) < 0) return r;
  InodeType stype, dtype;
  uint64_t sslot, dslot;
  if ((r = dir_find(sp, sname, &src, &stype, &sslot)) < 0) return r;
  if (stype == InodeType::kDir) {  // no moving a directory under itself
    for (uint32_t x = dp;;) {
      if (x == src) return -kEINVAL;
      if (x == kRootInode) break;
      Buffer* b;
      Inode* ip;
      if ((r = get_inode(x, &b, &ip)) < 0) return r;
      x = ip->parent;
      cache_.put(b);
    }
  }
  r = dir_find(dp, dname, &dst, &dtype, &dslot);
  bool replace = r == 0;
  if (r < 0 && r != -kENOENT) return r;
  if (replace) {
    if (dst == src) return 0;
    if (stype == InodeType::kDir && dtype != InodeType::kDir) return -kENOTDIR;
    if (stype != InodeType::kDir && dtype == InodeType::kDir) return -kEISDIR;
    if (dtype == InodeType::kDir) {
      bool empty;
      if ((r = dir_empty(dst, &empty)) < 0) return r;
      if (!empty) return -kENOTEMPTY;
    }
  }
  if ((r = begin(2 * kOpCredits)) < 0) return r;
  if (replace) {
    // Point the existing entry at the source: the name switches atomically.
    Buffer* ib;
    Inode* ip;
    if ((r = get_inode(dp, &ib, &ip)) < 0) return r;
    uint32_t block;
    bool fresh;
    r = bmap(ip, ib, dslot / kDirEntsPerBlock, false, &block, &fresh);
    cache_.put(ib);
    if (r < 0) return r;
    Buffer* db;
    if ((r = cache_.get(block, Mode::kRead, &db)) < 0) return r;
    auto* e = reinterpret_cast<DirEnt*>(db->data) + dslot % kDirEntsPerBlock;
    e->inode = src;
    e->type = static_cast<uint8_t>(stype);
    dirty_meta(db);
    cache_.put(db);
    if ((r = release_inode(dst)) < 0) return r;
  } else if ((r = dir_add(dp, dname, src, stype)) < 0) {
    return r;
  }
  if ((r = dir_remove_slot(sp, sslot)) < 0) return r;
  if (stype == InodeType::kDir && sp != dp) {
    Buffer* ib;
    Inode* ip;
    if ((r = get_inode(src, &ib, &ip)) < 0) return r;
    ip->parent = dp;
    dirty_meta(ib);
    cache_.put(ib);
  }
  return 0;
}

int ShaleFs::stat(uint32_t ino, InodeInfo* out) {
  Buffer* ib;
  Inode* ip;
  int r = get_inode(ino, &ib, &ip);
  if (r < 0) return r;
  if (ip->type == InodeType::kFree) {
    cache_.put(ib);
    return -kENOENT;
  }
  *out = {ip->type, ip->links, ip->size, ip->blocks, ip->mtime_ns};
  cache_.put(ib);
  return 0;
}

int64_t ShaleFs::read(uint32_t ino, uint64_t offset, void* buf, uint64_t n) {
  Buffer* ib;
  Inode* ip;
  int r = get_inode(ino, &ib, &ip);
  if (r < 0) return r;
  if (ip->type != InodeType::kFile) {
    cache_.put(ib);
    return ip->type == InodeType::kDir ? -kEISDIR : -kENOENT;
  }
  if (offset >= ip->size) {
    cache_.put(ib);
    return 0;
  }
  n = shale::min(n, ip->size - offset);
  auto* out = static_cast<uint8_t*>(buf);
  uint64_t done = 0;
  while (done < n) {
    uint64_t pos = offset + done, within = pos % kBlockSize;
    uint64_t len = shale::min<uint64_t>(kBlockSize - within, n - done);
    uint32_t block;
    bool fresh;
    if ((r = bmap(ip, ib, pos / kBlockSize, false, &block, &fresh)) < 0) break;
    if (!block) {
      memset(out + done, 0, len);  // hole
    } else {
      Buffer* db;
      if ((r = cache_.get(block, Mode::kRead, &db)) < 0) break;
      memcpy(out + done, db->data + within, len);
      cache_.put(db);
    }
    done += len;
  }
  cache_.put(ib);
  return done ? static_cast<int64_t>(done) : r;
}

int64_t ShaleFs::write(uint32_t ino, uint64_t offset, const void* buf, uint64_t n) {
  if (n == 0) return 0;
  uint64_t end;
  if (__builtin_add_overflow(offset, n, &end) || end > kMaxFileBlocks * kBlockSize) return -kEFBIG;
  auto* in = static_cast<const uint8_t*>(buf);
  constexpr uint64_t kChunkBlocks = 32;
  uint64_t done = 0;
  int r = 0;
  while (done < n && r == 0) {
    if ((r = begin(kOpCredits)) < 0) break;
    Buffer* ib;
    Inode* ip;
    if ((r = get_inode(ino, &ib, &ip)) < 0) break;
    if (ip->type != InodeType::kFile) {
      cache_.put(ib);
      r = ip->type == InodeType::kDir ? -kEISDIR : -kENOENT;
      break;
    }
    uint64_t chunk_end = shale::min(n, done + kChunkBlocks * kBlockSize);
    while (done < chunk_end) {
      uint64_t pos = offset + done, within = pos % kBlockSize;
      uint64_t len = shale::min<uint64_t>(kBlockSize - within, chunk_end - done);
      uint32_t block;
      bool fresh;
      if ((r = bmap(ip, ib, pos / kBlockSize, true, &block, &fresh)) < 0) break;
      Buffer* db;
      bool whole = within == 0 && len == kBlockSize;
      if ((r = cache_.get(block, fresh || whole ? Mode::kFresh : Mode::kRead, &db)) < 0) break;
      memcpy(db->data + within, in + done, len);
      dirty_data(db);
      cache_.put(db);
      done += len;
    }
    if (offset + done > ip->size) ip->size = offset + done;
    ip->mtime_ns = now();
    dirty_meta(ib);
    cache_.put(ib);
  }
  return done ? static_cast<int64_t>(done) : r;
}

int ShaleFs::truncate(uint32_t ino, uint64_t size) {
  if (size > kMaxFileBlocks * kBlockSize) return -kEFBIG;
  Buffer* ib;
  Inode* ip;
  int r = begin(kOpCredits);
  if (r < 0) return r;
  if ((r = get_inode(ino, &ib, &ip)) < 0) return r;
  if (ip->type != InodeType::kFile) {
    cache_.put(ib);
    return ip->type == InodeType::kDir ? -kEISDIR : -kENOENT;
  }
  if (size < ip->size) {
    uint64_t keep = (size + kBlockSize - 1) / kBlockSize;
    if ((r = free_blocks_from(ip, ib, keep)) < 0) {
      cache_.put(ib);
      return r;
    }
    if (size % kBlockSize) {  // zero the tail so a later extension reads zeros
      uint32_t block;
      bool fresh;
      if ((r = bmap(ip, ib, size / kBlockSize, false, &block, &fresh)) == 0 && block) {
        Buffer* db;
        if ((r = cache_.get(block, Mode::kRead, &db)) == 0) {
          memset(db->data + size % kBlockSize, 0, kBlockSize - size % kBlockSize);
          dirty_data(db);
          cache_.put(db);
        }
      }
    }
  }
  ip->size = size;
  ip->mtime_ns = now();
  dirty_meta(ib);
  cache_.put(ib);
  return r;
}

}  // namespace shale::fs
