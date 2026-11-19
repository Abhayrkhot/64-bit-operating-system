// Kernel heap: slab caches for small objects, exact-size page runs for large
// ones, both carved out of a BuddyAllocator.
//
// Small requests (<= 2048 bytes) round up to one of 14 size classes. Fixed-size
// kernel objects (threads, inodes, buffers) get dedicated caches of their exact
// size instead (create_cache, like Linux's kmem_cache_create), which avoids
// size-class rounding. Every cache owns slabs: naturally aligned
// 2^order-frame blocks with a 64-byte header followed by equal-sized objects.
// The slab order is the smallest one that keeps per-slab waste (header +
// leftover) at or under 1/16. Objects carry no per-object header; free() finds
// the slab from the frame's owner tag (FrameMeta::aux) and aligning down to
// the slab size.
//
// Large requests take exactly ceil(size / 4096) frames via alloc_exact.
//
// Portable: the kernel maps frame i at HHDM + i*4096; the host benchmark
// passes an aligned arena. Not internally synchronized.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mm/buddy.hpp"

namespace shale::mm {

class Heap {
 public:
  static constexpr size_t kMaxSlabObject = 2048;
  static constexpr unsigned kNumClasses = 14;
  static constexpr uint32_t kClassSizes[kNumClasses] = {16,  32,  48,  64,  96,   128,  192,
                                                        256, 384, 512, 768, 1024, 1536, 2048};
  static constexpr size_t kSlabHeader = 64;
  static constexpr unsigned kMaxCaches = 48;  // 14 size classes + dedicated caches

  struct ClassStats {
    uint32_t object_size;
    uint32_t slab_frames;  // frames per slab
    uint32_t objects_per_slab;
    uint64_t slabs;        // slabs currently held (partial + full + empty)
    uint64_t live_objects;
  };

  struct Stats {
    uint64_t slab_frames;   // frames held by slabs, including cached empty slabs
    uint64_t large_frames;  // frames held by large allocations
    uint64_t live_objects;  // slab objects + large allocations outstanding
    uint64_t failed_allocs;
  };

  // frame0: address at which frame index 0 would be mapped.
  void init(BuddyAllocator* buddy, uint8_t* frame0);

  void* alloc(size_t size);
  // Creates a dedicated cache for objects of exactly `object_size` bytes
  // (rounded up to 8). Returns its id, or -1 if out of cache slots.
  int create_cache(uint32_t object_size);
  void* alloc_from(int cache);
  // Returns false (and does nothing) for pointers this heap did not hand out
  // or that are already free when detectable.
  bool free(void* p);
  size_t usable_size(const void* p) const;

  Stats stats() const;
  ClassStats class_stats(unsigned index) const;
  uint64_t footprint_bytes() const;
  bool check_invariants() const;

  // Empty slabs kept per class before returning frames to the buddy allocator.
  void set_empty_slab_limit(uint32_t n) { empty_limit_ = n; }

  static unsigned class_index(size_t size);

 private:
  struct Slab {
    Slab* next;
    Slab* prev;
    void* free_list;
    uint16_t inuse;
    uint16_t capacity;
    uint32_t class_index;
    uint64_t magic;
  };
  struct List {
    Slab* head = nullptr;
    uint64_t count = 0;
    void push(Slab* s);
    void remove(Slab* s);
  };
  struct Cache {
    uint32_t object_size = 0;
    uint8_t order = 0;
    uint16_t capacity = 0;
    List partial, full, empty;
    uint64_t live = 0;
  };

  static constexpr uint64_t kSlabMagic = 0x534c4142534c4142ull;  // "SLABSLAB"
  static constexpr uint16_t kTagSlab = 1, kTagLarge = 2;

  Slab* new_slab(unsigned ci);
  void setup_cache(unsigned ci, uint32_t object_size);
  void release_slab(Slab* s);
  uint32_t frame_of(const void* p) const;
  void tag_frames(uint32_t frame, uint32_t count, uint16_t tag);

  BuddyAllocator* buddy_ = nullptr;
  uint8_t* frame0_ = nullptr;
  Cache caches_[kMaxCaches];
  unsigned num_caches_ = kNumClasses;
  uint32_t empty_limit_ = 1;
  uint64_t slab_frames_ = 0;
  uint64_t large_frames_ = 0;
  uint64_t large_live_ = 0;
  uint64_t failed_ = 0;
};

}  // namespace shale::mm
