// Binary buddy allocator over physical page frames.
//
// Frames are identified by absolute index (physical address >> 12), so block
// alignment in index space is physical alignment and a block's buddy is
// `frame ^ (1 << order)`. Metadata lives in a caller-provided array with one
// 12-byte FrameMeta per frame; free blocks are kept on per-order doubly linked
// lists threaded through that array, so the allocator never touches the
// memory it manages (the host benchmark relies on this).
//
// Not internally synchronized: the kernel wraps it in a lock.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace shale::mm {

enum class FrameState : uint8_t {
  kReserved = 0,   // not managed (firmware, MMIO hole, allocator metadata)
  kFreeHead = 1,   // first frame of a free block of `order`
  kAllocHead = 2,  // first frame of an allocated block of `order`
  kTail = 3,       // inside a block, not its first frame
};

struct FrameMeta {
  uint32_t next;
  uint32_t prev;
  uint8_t order;
  FrameState state;
  uint16_t aux;  // owner tag for the layer above (the heap uses it to find slabs)
};
static_assert(sizeof(FrameMeta) == 12, "FrameMeta should stay small");

class BuddyAllocator {
 public:
  static constexpr unsigned kMaxOrder = 10;  // largest block: 1024 frames = 4 MiB
  static constexpr uint32_t kNone = 0xFFFFFFFFu;

  // `meta` must hold `frame_count` entries. All frames start reserved.
  void init(FrameMeta* meta, uint32_t frame_count);

  // Hands [first, first + count) to the allocator, merging with free neighbors.
  void add_free_range(uint32_t first, uint32_t count);

  // Returns the first frame of a naturally aligned 2^order block, or kNone.
  uint32_t alloc(unsigned order);

  // Frees a block returned by alloc(order). Detects double frees and order
  // mismatches; returns false (and changes nothing) if the call is invalid.
  bool free(uint32_t frame, unsigned order);

  // Allocates exactly `count` contiguous frames (1..2^kMaxOrder) out of one
  // naturally aligned block and returns the unused tail to the free lists,
  // so a 5-frame request costs 5 frames instead of 8.
  uint32_t alloc_exact(uint32_t count);
  bool free_exact(uint32_t frame, uint32_t count);

  uint64_t free_frames() const { return free_frames_; }
  uint64_t managed_frames() const { return managed_frames_; }
  uint32_t free_blocks(unsigned order) const { return nfree_[order]; }
  // Largest order with a free block, or -1 if nothing is free.
  int largest_free_order() const;
  // Frames sitting in free blocks of at least `order` (usable for an order-k request).
  uint64_t free_frames_at_or_above(unsigned order) const;

  uint32_t frame_count() const { return count_; }
  FrameMeta& meta(uint32_t frame) { return meta_[frame]; }
  const FrameMeta& meta(uint32_t frame) const { return meta_[frame]; }

  // Walks every list and recounts; returns false on any inconsistency.
  bool check_invariants() const;

 private:
  void push(unsigned order, uint32_t frame);
  void unlink(unsigned order, uint32_t frame);

  FrameMeta* meta_ = nullptr;
  uint32_t count_ = 0;
  uint32_t heads_[kMaxOrder + 1] = {};
  uint32_t nfree_[kMaxOrder + 1] = {};
  uint64_t free_frames_ = 0;
  uint64_t managed_frames_ = 0;
};

}  // namespace shale::mm
