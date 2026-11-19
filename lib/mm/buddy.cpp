#include "mm/buddy.hpp"

namespace shale::mm {

void BuddyAllocator::init(FrameMeta* meta, uint32_t frame_count) {
  meta_ = meta;
  count_ = frame_count;
  for (uint32_t i = 0; i < frame_count; i++) meta_[i] = {kNone, kNone, 0, FrameState::kReserved, 0};
  for (unsigned o = 0; o <= kMaxOrder; o++) {
    heads_[o] = kNone;
    nfree_[o] = 0;
  }
  free_frames_ = 0;
  managed_frames_ = 0;
}

void BuddyAllocator::push(unsigned order, uint32_t frame) {
  FrameMeta& m = meta_[frame];
  m.state = FrameState::kFreeHead;
  m.order = static_cast<uint8_t>(order);
  m.prev = kNone;
  m.next = heads_[order];
  if (heads_[order] != kNone) meta_[heads_[order]].prev = frame;
  heads_[order] = frame;
  nfree_[order]++;
}

void BuddyAllocator::unlink(unsigned order, uint32_t frame) {
  FrameMeta& m = meta_[frame];
  if (m.prev != kNone)
    meta_[m.prev].next = m.next;
  else
    heads_[order] = m.next;
  if (m.next != kNone) meta_[m.next].prev = m.prev;
  m.next = m.prev = kNone;
  nfree_[order]--;
}

void BuddyAllocator::add_free_range(uint32_t first, uint32_t count) {
  if (first >= count_) return;
  if (count > count_ - first) count = count_ - first;
  uint32_t f = first, end = first + count;
  while (f < end) {
    unsigned order = kMaxOrder;
    while (order > 0 && ((f & ((1u << order) - 1)) != 0 || f + (1u << order) > end)) order--;
    for (uint32_t i = 1; i < (1u << order); i++) meta_[f + i].state = FrameState::kTail;
    meta_[f].state = FrameState::kAllocHead;
    meta_[f].order = static_cast<uint8_t>(order);
    managed_frames_ += 1u << order;
    free(f, order);
    f += 1u << order;
  }
}

uint32_t BuddyAllocator::alloc(unsigned order) {
  if (order > kMaxOrder) return kNone;
  unsigned o = order;
  while (o <= kMaxOrder && heads_[o] == kNone) o++;
  if (o > kMaxOrder) return kNone;
  uint32_t frame = heads_[o];
  unlink(o, frame);
  while (o > order) {  // split, keeping the lower half
    o--;
    push(o, frame + (1u << o));
  }
  meta_[frame].state = FrameState::kAllocHead;
  meta_[frame].order = static_cast<uint8_t>(order);
  free_frames_ -= 1u << order;
  return frame;
}

bool BuddyAllocator::free(uint32_t frame, unsigned order) {
  if (frame >= count_ || order > kMaxOrder || (frame & ((1u << order) - 1)) != 0) return false;
  FrameMeta& m = meta_[frame];
  if (m.state != FrameState::kAllocHead || m.order != order) return false;
  m.aux = 0;
  free_frames_ += 1u << order;
  while (order < kMaxOrder) {
    uint32_t buddy = frame ^ (1u << order);
    if (buddy >= count_) break;
    const FrameMeta& b = meta_[buddy];
    if (b.state != FrameState::kFreeHead || b.order != order) break;
    unlink(order, buddy);
    uint32_t upper = frame > buddy ? frame : buddy;
    meta_[upper].state = FrameState::kTail;
    frame = frame < buddy ? frame : buddy;
    order++;
  }
  push(order, frame);
  return true;
}

namespace {
// Greedy decomposition of [frame, frame + count) into naturally aligned
// power-of-two pieces. Deterministic, so alloc_exact and free_exact agree.
template <typename Fn>
void for_each_piece(uint32_t frame, uint32_t count, Fn fn) {
  uint32_t f = frame, end = frame + count;
  while (f < end) {
    unsigned order = BuddyAllocator::kMaxOrder;
    while (order > 0 && ((f & ((1u << order) - 1)) != 0 || f + (1u << order) > end)) order--;
    fn(f, order);
    f += 1u << order;
  }
}
unsigned ceil_log2(uint32_t n) {
  unsigned o = 0;
  while ((1u << o) < n) o++;
  return o;
}
}  // namespace

uint32_t BuddyAllocator::alloc_exact(uint32_t count) {
  if (count == 0 || count > (1u << kMaxOrder)) return kNone;
  unsigned order = ceil_log2(count);
  uint32_t frame = alloc(order);
  if (frame == kNone || count == (1u << order)) return frame;
  // Re-describe the block as allocated pieces, then free the tail pieces.
  uint32_t block_end = frame + (1u << order);
  auto mark = [this](uint32_t f, unsigned o) {
    meta_[f].state = FrameState::kAllocHead;
    meta_[f].order = static_cast<uint8_t>(o);
  };
  for_each_piece(frame, count, mark);
  for_each_piece(frame + count, block_end - (frame + count), [&](uint32_t f, unsigned o) {
    mark(f, o);
    free(f, o);
  });
  return frame;
}

bool BuddyAllocator::free_exact(uint32_t frame, uint32_t count) {
  if (count == 0 || count > (1u << kMaxOrder) || frame >= count_ || count > count_ - frame) return false;
  bool ok = true;
  for_each_piece(frame, count, [&](uint32_t f, unsigned o) {
    if (meta_[f].state != FrameState::kAllocHead || meta_[f].order != o) ok = false;
  });
  if (!ok) return false;
  for_each_piece(frame, count, [&](uint32_t f, unsigned o) { free(f, o); });
  return true;
}

int BuddyAllocator::largest_free_order() const {
  for (int o = kMaxOrder; o >= 0; o--)
    if (heads_[o] != kNone) return o;
  return -1;
}

uint64_t BuddyAllocator::free_frames_at_or_above(unsigned order) const {
  uint64_t n = 0;
  for (unsigned o = order; o <= kMaxOrder; o++) n += static_cast<uint64_t>(nfree_[o]) << o;
  return n;
}

bool BuddyAllocator::check_invariants() const {
  uint64_t total = 0;
  for (unsigned o = 0; o <= kMaxOrder; o++) {
    uint32_t n = 0, prev = kNone;
    for (uint32_t f = heads_[o]; f != kNone; f = meta_[f].next) {
      if (f >= count_ || meta_[f].state != FrameState::kFreeHead || meta_[f].order != o) return false;
      if (meta_[f].prev != prev || (f & ((1u << o) - 1)) != 0) return false;
      // A free block's buddy must not also be a free head of the same order
      // (they would have been merged).
      uint32_t buddy = f ^ (1u << o);
      if (o < kMaxOrder && buddy < count_ && meta_[buddy].state == FrameState::kFreeHead &&
          meta_[buddy].order == o)
        return false;
      prev = f;
      if (++n > count_) return false;  // cycle
    }
    if (n != nfree_[o]) return false;
    total += static_cast<uint64_t>(n) << o;
  }
  return total == free_frames_;
}

}  // namespace shale::mm
