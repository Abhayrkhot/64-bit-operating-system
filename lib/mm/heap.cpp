#include "mm/heap.hpp"

namespace os::mm {
namespace {
constexpr uint64_t kFrame = 4096;
constexpr unsigned kMaxSlabOrder = 3;

uint16_t make_tag(uint16_t kind, uint16_t value) { return static_cast<uint16_t>((kind << 12) | (value & 0xFFF)); }
uint16_t tag_kind(uint16_t tag) { return tag >> 12; }
uint16_t tag_value(uint16_t tag) { return tag & 0xFFF; }

// Lookup table: (size + 15) / 16 -> class index, for size <= 2048.
struct ClassTable {
  uint8_t index[Heap::kMaxSlabObject / 16 + 1];
  constexpr ClassTable() : index() {
    unsigned ci = 0;
    for (unsigned g = 0; g <= Heap::kMaxSlabObject / 16; g++) {
      while (Heap::kClassSizes[ci] < g * 16) ci++;
      index[g] = static_cast<uint8_t>(ci);
    }
  }
};
constexpr ClassTable kClassTable;
}  // namespace

unsigned Heap::class_index(size_t size) { return kClassTable.index[(size + 15) / 16]; }

void Heap::List::push(Slab* s) {
  s->prev = nullptr;
  s->next = head;
  if (head) head->prev = s;
  head = s;
  count++;
}

void Heap::List::remove(Slab* s) {
  if (s->prev)
    s->prev->next = s->next;
  else
    head = s->next;
  if (s->next) s->next->prev = s->prev;
  s->next = s->prev = nullptr;
  count--;
}

void Heap::setup_cache(unsigned ci, uint32_t object_size) {
  Cache& c = caches_[ci];
  c = Cache();
  c.object_size = object_size;
  // Smallest slab order with waste <= 1/16; failing that, the least wasteful.
  uint64_t best_waste_ppm = ~0ull;
  for (unsigned order = 0; order <= kMaxSlabOrder; order++) {
    uint64_t bytes = kFrame << order;
    uint64_t n = (bytes - kSlabHeader) / object_size;
    if (n == 0) continue;
    uint64_t waste = bytes - n * object_size;  // header + leftover
    uint64_t ppm = waste * 1000000 / bytes;
    if (ppm < best_waste_ppm) {
      best_waste_ppm = ppm;
      c.order = static_cast<uint8_t>(order);
      c.capacity = static_cast<uint16_t>(n);
    }
    if (waste * 16 <= bytes) {
      c.order = static_cast<uint8_t>(order);
      c.capacity = static_cast<uint16_t>(n);
      break;
    }
  }
}

void Heap::init(BuddyAllocator* buddy, uint8_t* frame0) {
  buddy_ = buddy;
  frame0_ = frame0;
  num_caches_ = kNumClasses;
  for (unsigned ci = 0; ci < kNumClasses; ci++) setup_cache(ci, kClassSizes[ci]);
}

int Heap::create_cache(uint32_t object_size) {
  if (num_caches_ == kMaxCaches || object_size == 0 || object_size > kMaxSlabObject * 2) return -1;
  object_size = (object_size + 7) & ~7u;
  if (object_size < sizeof(void*)) object_size = sizeof(void*);
  setup_cache(num_caches_, object_size);
  return static_cast<int>(num_caches_++);
}

uint32_t Heap::frame_of(const void* p) const {
  return static_cast<uint32_t>((static_cast<const uint8_t*>(p) - frame0_) / kFrame);
}

void Heap::tag_frames(uint32_t frame, uint32_t count, uint16_t tag) {
  for (uint32_t i = 0; i < count; i++) buddy_->meta(frame + i).aux = tag;
}

Heap::Slab* Heap::new_slab(unsigned ci) {
  Cache& c = caches_[ci];
  uint32_t frame = buddy_->alloc(c.order);
  if (frame == BuddyAllocator::kNone) return nullptr;
  tag_frames(frame, 1u << c.order, make_tag(kTagSlab, static_cast<uint16_t>((ci << 4) | c.order)));
  slab_frames_ += 1u << c.order;
  auto* s = reinterpret_cast<Slab*>(frame0_ + static_cast<uint64_t>(frame) * kFrame);
  s->next = s->prev = nullptr;
  s->inuse = 0;
  s->capacity = c.capacity;
  s->class_index = ci;
  s->magic = kSlabMagic;
  // Thread the free list in address order so fresh slabs hand out ascending addresses.
  uint8_t* base = reinterpret_cast<uint8_t*>(s) + kSlabHeader;
  s->free_list = base;
  for (uint32_t i = 0; i < c.capacity; i++) {
    void* next = i + 1 < c.capacity ? base + (i + 1) * c.object_size : nullptr;
    *reinterpret_cast<void**>(base + i * c.object_size) = next;
  }
  return s;
}

void Heap::release_slab(Slab* s) {
  Cache& c = caches_[s->class_index];
  uint32_t frame = frame_of(s);
  s->magic = 0;
  tag_frames(frame, 1u << c.order, 0);
  slab_frames_ -= 1u << c.order;
  buddy_->free(frame, c.order);
}

void* Heap::alloc_from(int cache) {
  if (cache < 0 || static_cast<unsigned>(cache) >= num_caches_) return nullptr;
  {
    unsigned ci = static_cast<unsigned>(cache);
    Cache& c = caches_[ci];
    Slab* s = c.partial.head;
    if (!s) {
      if ((s = c.empty.head) != nullptr) {
        c.empty.remove(s);
      } else if ((s = new_slab(ci)) == nullptr) {
        failed_++;
        return nullptr;
      }
      c.partial.push(s);
    }
    void* obj = s->free_list;
    s->free_list = *static_cast<void**>(obj);
    s->inuse++;
    c.live++;
    if (s->inuse == s->capacity) {
      c.partial.remove(s);
      c.full.push(s);
    }
    return obj;
  }
}

void* Heap::alloc(size_t size) {
  if (size == 0) size = 1;
  if (size <= kMaxSlabObject) return alloc_from(static_cast<int>(class_index(size)));
  uint64_t frames = (size + kFrame - 1) / kFrame;
  if (frames > (1u << BuddyAllocator::kMaxOrder)) {
    failed_++;
    return nullptr;
  }
  uint32_t frame = buddy_->alloc_exact(static_cast<uint32_t>(frames));
  if (frame == BuddyAllocator::kNone) {
    failed_++;
    return nullptr;
  }
  buddy_->meta(frame).aux = make_tag(kTagLarge, static_cast<uint16_t>(frames));
  large_frames_ += frames;
  large_live_++;
  return frame0_ + static_cast<uint64_t>(frame) * kFrame;
}

bool Heap::free(void* p) {
  if (!p) return true;
  auto* bytes = static_cast<uint8_t*>(p);
  if (bytes < frame0_) return false;
  uint32_t frame = frame_of(p);
  if (frame >= buddy_->frame_count()) return false;
  uint16_t tag = buddy_->meta(frame).aux;
  if (tag_kind(tag) == kTagLarge) {
    if ((bytes - frame0_) % kFrame != 0) return false;
    uint32_t frames = tag_value(tag);
    buddy_->meta(frame).aux = 0;
    if (!buddy_->free_exact(frame, frames)) return false;
    large_frames_ -= frames;
    large_live_--;
    return true;
  }
  if (tag_kind(tag) != kTagSlab) return false;
  unsigned ci = tag_value(tag) >> 4, order = tag_value(tag) & 0xF;
  Cache& c = caches_[ci];
  uint64_t slab_bytes = kFrame << order;
  uint64_t offset = static_cast<uint64_t>(bytes - frame0_);
  auto* s = reinterpret_cast<Slab*>(frame0_ + (offset & ~(slab_bytes - 1)));
  uint64_t in_slab = offset & (slab_bytes - 1);
  if (s->magic != kSlabMagic || in_slab < kSlabHeader || (in_slab - kSlabHeader) % c.object_size != 0 ||
      (in_slab - kSlabHeader) / c.object_size >= c.capacity || s->inuse == 0)
    return false;
  bool was_full = s->inuse == s->capacity;
  *static_cast<void**>(p) = s->free_list;
  s->free_list = p;
  s->inuse--;
  c.live--;
  if (was_full) {
    c.full.remove(s);
    c.partial.push(s);
  }
  if (s->inuse == 0) {
    c.partial.remove(s);
    if (c.empty.count < empty_limit_)
      c.empty.push(s);
    else
      release_slab(s);
  }
  return true;
}

size_t Heap::usable_size(const void* p) const {
  uint32_t frame = frame_of(p);
  uint16_t tag = buddy_->meta(frame).aux;
  if (tag_kind(tag) == kTagLarge) return tag_value(tag) * kFrame;
  if (tag_kind(tag) == kTagSlab) return caches_[tag_value(tag) >> 4].object_size;
  return 0;
}

Heap::Stats Heap::stats() const {
  Stats st{slab_frames_, large_frames_, large_live_, failed_};
  for (unsigned ci = 0; ci < num_caches_; ci++) st.live_objects += caches_[ci].live;
  return st;
}

Heap::ClassStats Heap::class_stats(unsigned i) const {
  const Cache& c = caches_[i];
  return {c.object_size, 1u << c.order, c.capacity, c.partial.count + c.full.count + c.empty.count, c.live};
}

uint64_t Heap::footprint_bytes() const { return (slab_frames_ + large_frames_) * kFrame; }

bool Heap::check_invariants() const {
  uint64_t frames = 0;
  for (unsigned ci = 0; ci < num_caches_; ci++) {
    const Cache& c = caches_[ci];
    uint64_t live = 0;
    const List* lists[3] = {&c.partial, &c.full, &c.empty};
    for (int li = 0; li < 3; li++) {
      uint64_t n = 0;
      for (const Slab* s = lists[li]->head; s; s = s->next) {
        if (s->magic != kSlabMagic || s->class_index != ci || s->capacity != c.capacity) return false;
        if (li == 0 && (s->inuse == 0 || s->inuse == s->capacity)) return false;
        if (li == 1 && s->inuse != s->capacity) return false;
        if (li == 2 && s->inuse != 0) return false;
        uint32_t free_count = 0;
        for (void* o = s->free_list; o; o = *static_cast<void**>(o))
          if (++free_count > s->capacity) return false;
        if (free_count + s->inuse != s->capacity) return false;
        live += s->inuse;
        frames += 1u << c.order;
        n++;
      }
      if (n != lists[li]->count) return false;
    }
    if (live != c.live) return false;
  }
  return frames == slab_frames_;
}

}  // namespace os::mm
