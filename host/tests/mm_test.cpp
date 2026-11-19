// Host tests for the buddy allocator and slab heap: the exact code the kernel
// runs, checked against shadow models under random workloads.
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "common/test.hpp"
#include "mm/buddy.hpp"
#include "mm/heap.hpp"
#include "util/random.hpp"

using shale::mm::BuddyAllocator;
using shale::mm::FrameMeta;
using shale::mm::Heap;

namespace {

struct BuddyFixture {
  std::vector<FrameMeta> meta;
  BuddyAllocator buddy;
  explicit BuddyFixture(uint32_t frames) : meta(frames) { buddy.init(meta.data(), frames); }
};

struct Arena {
  uint32_t frames;
  uint8_t* mem;
  std::vector<FrameMeta> meta;
  BuddyAllocator buddy;
  Heap heap;
  explicit Arena(uint32_t n) : frames(n), meta(n) {
    mem = static_cast<uint8_t*>(std::aligned_alloc(4 << 20, static_cast<size_t>(n) * 4096));
    buddy.init(meta.data(), n);
    buddy.add_free_range(1, n - 1);  // like the kernel, frame 0 is never handed out
    heap.init(&buddy, mem);
  }
  ~Arena() { std::free(mem); }
};

}  // namespace

TEST(buddy_initial_ranges_merge_to_max_blocks) {
  BuddyFixture f(4096);
  f.buddy.add_free_range(0, 2048);
  f.buddy.add_free_range(2048, 2048);
  EXPECT_EQ(f.buddy.free_frames(), 4096u);
  EXPECT_EQ(f.buddy.free_blocks(BuddyAllocator::kMaxOrder), 4u);
  EXPECT(f.buddy.check_invariants());
}

TEST(buddy_unaligned_range_splits_into_aligned_blocks) {
  BuddyFixture f(4096);
  f.buddy.add_free_range(3, 1000);  // [3, 1003)
  EXPECT_EQ(f.buddy.free_frames(), 1000u);
  EXPECT(f.buddy.check_invariants());
  // No aligned 512-frame block fits inside [3, 1003); two aligned 256-frame blocks do.
  EXPECT_EQ(f.buddy.alloc(9), BuddyAllocator::kNone);
  uint32_t a = f.buddy.alloc(8), b = f.buddy.alloc(8);
  EXPECT((a == 256u && b == 512u) || (a == 512u && b == 256u));
  EXPECT_EQ(f.buddy.alloc(8), BuddyAllocator::kNone);
}

TEST(buddy_rejects_double_free_and_bad_order) {
  BuddyFixture f(1024);
  f.buddy.add_free_range(0, 1024);
  uint32_t a = f.buddy.alloc(2);
  EXPECT(f.buddy.free(a, 2));
  EXPECT(!f.buddy.free(a, 2));
  uint32_t b = f.buddy.alloc(3);
  EXPECT(!f.buddy.free(b, 2));
  EXPECT(!f.buddy.free(b + 1, 0));
  EXPECT(f.buddy.free(b, 3));
  EXPECT_EQ(f.buddy.free_frames(), 1024u);
  EXPECT(f.buddy.check_invariants());
}

TEST(buddy_random_workload_matches_shadow) {
  constexpr uint32_t kFrames = 1 << 16;
  BuddyFixture f(kFrames);
  f.buddy.add_free_range(0, kFrames);
  std::vector<uint8_t> owned(kFrames, 0);
  struct Block {
    uint32_t frame, count;
    bool exact;
  };
  std::vector<Block> live;
  shale::Random rng(1234);
  for (int step = 0; step < 300000; step++) {
    if (!live.empty() && (rng.chance(1, 2) || f.buddy.free_frames() < 2048)) {
      size_t i = rng.below(live.size());
      Block b = live[i];
      live[i] = live.back();
      live.pop_back();
      for (uint32_t k = 0; k < b.count; k++) owned[b.frame + k] = 0;
      unsigned order = 0;
      while ((1u << order) < b.count) order++;
      EXPECT(b.exact ? f.buddy.free_exact(b.frame, b.count) : f.buddy.free(b.frame, order));
    } else {
      bool exact = rng.chance(1, 3);
      uint32_t count;
      uint32_t frame;
      if (exact) {
        count = 1 + static_cast<uint32_t>(rng.below(rng.chance(1, 10) ? 700 : 12));
        frame = f.buddy.alloc_exact(count);
      } else {
        unsigned order = static_cast<unsigned>(rng.below(rng.chance(1, 10) ? 11 : 4));
        count = 1u << order;
        frame = f.buddy.alloc(order);
        if (frame != BuddyAllocator::kNone) EXPECT((frame & (count - 1)) == 0);
      }
      if (frame == BuddyAllocator::kNone) continue;
      for (uint32_t k = 0; k < count; k++) {
        EXPECT(owned[frame + k] == 0);  // no overlap with any live block
        owned[frame + k] = 1;
      }
      live.push_back({frame, count, exact});
    }
    if (step % 20000 == 0) EXPECT(f.buddy.check_invariants());
  }
  uint64_t live_frames = 0;
  for (const Block& b : live) live_frames += b.count;
  EXPECT_EQ(f.buddy.free_frames(), kFrames - live_frames);
  for (const Block& b : live) {
    unsigned order = 0;
    while ((1u << order) < b.count) order++;
    EXPECT(b.exact ? f.buddy.free_exact(b.frame, b.count) : f.buddy.free(b.frame, order));
  }
  EXPECT_EQ(f.buddy.free_frames(), kFrames);
  EXPECT_EQ(f.buddy.free_blocks(BuddyAllocator::kMaxOrder), kFrames >> BuddyAllocator::kMaxOrder);
  EXPECT(f.buddy.check_invariants());
}

TEST(buddy_alloc_exact_returns_tail) {
  BuddyFixture f(1024);
  f.buddy.add_free_range(0, 1024);
  uint32_t a = f.buddy.alloc_exact(5);
  EXPECT(a != BuddyAllocator::kNone);
  EXPECT_EQ(f.buddy.free_frames(), 1019u);  // 5 used, not 8
  EXPECT(f.buddy.check_invariants());
  EXPECT(f.buddy.free_exact(a, 5));
  EXPECT_EQ(f.buddy.free_blocks(BuddyAllocator::kMaxOrder), 1u);
}

TEST(heap_class_table_covers_all_sizes) {
  for (size_t s = 1; s <= Heap::kMaxSlabObject; s++) {
    unsigned ci = Heap::class_index(s);
    EXPECT(ci < Heap::kNumClasses);
    EXPECT(Heap::kClassSizes[ci] >= s);
    if (ci > 0) EXPECT(Heap::kClassSizes[ci - 1] < s);
  }
}

TEST(heap_slab_geometry_waste_at_most_one_sixteenth) {
  Arena a(4096);
  for (unsigned ci = 0; ci < Heap::kNumClasses; ci++) {
    auto cs = a.heap.class_stats(ci);
    uint64_t bytes = cs.slab_frames * 4096ull;
    uint64_t waste = bytes - cs.objects_per_slab * static_cast<uint64_t>(cs.object_size);
    EXPECT(waste * 16 <= bytes);
  }
}

TEST(heap_typed_caches_use_exact_sizes) {
  Arena a(4096);
  int inode = a.heap.create_cache(616);
  int node = a.heap.create_cache(37);  // rounds up to 40
  ASSERT(inode >= 0 && node >= 0 && inode != node);
  EXPECT_EQ(a.heap.class_stats(static_cast<unsigned>(node)).object_size, 40u);
  std::vector<void*> objs;
  for (int i = 0; i < 1000; i++) {
    void* p = a.heap.alloc_from(i % 2 ? inode : node);
    ASSERT(p != nullptr);
    std::memset(p, i, i % 2 ? 616 : 37);
    EXPECT_EQ(a.heap.usable_size(p), i % 2 ? 616u : 40u);
    objs.push_back(p);
  }
  EXPECT(a.heap.check_invariants());
  for (void* p : objs) EXPECT(a.heap.free(p));
  EXPECT(a.heap.check_invariants());
  EXPECT(a.heap.alloc_from(999) == nullptr);
}

TEST(heap_random_workload_matches_shadow) {
  Arena a(1 << 16);  // 256 MiB
  shale::Random rng(99);
  std::map<uintptr_t, size_t> live;  // address -> requested size
  std::vector<uintptr_t> order;
  for (int step = 0; step < 400000; step++) {
    if (!order.empty() && rng.chance(45, 100)) {
      size_t i = rng.below(order.size());
      uintptr_t p = order[i];
      order[i] = order.back();
      order.pop_back();
      size_t size = live[p];
      auto* bytes = reinterpret_cast<uint8_t*>(p);
      for (size_t k = 0; k < size; k += 97) EXPECT(bytes[k] == static_cast<uint8_t>((p >> 4) + k));
      EXPECT(a.heap.free(bytes));
      live.erase(p);
    } else {
      size_t size = rng.chance(1, 20) ? 1 + rng.below(50000) : 1 + rng.below(2200);
      auto* p = static_cast<uint8_t*>(a.heap.alloc(size));
      ASSERT(p != nullptr);
      auto addr = reinterpret_cast<uintptr_t>(p);
      EXPECT(addr % 16 == 0);
      EXPECT(a.heap.usable_size(p) >= size);
      // No overlap with neighbours in address order.
      auto next = live.lower_bound(addr);
      if (next != live.end()) EXPECT(addr + size <= next->first);
      if (next != live.begin()) {
        auto prev = std::prev(next);
        EXPECT(prev->first + prev->second <= addr);
      }
      live[addr] = size;
      order.push_back(addr);
      for (size_t k = 0; k < size; k += 97) p[k] = static_cast<uint8_t>((addr >> 4) + k);
    }
    if (step % 50000 == 0) EXPECT(a.heap.check_invariants() && a.buddy.check_invariants());
  }
  for (uintptr_t p : order) EXPECT(a.heap.free(reinterpret_cast<void*>(p)));
  EXPECT(a.heap.check_invariants());
  EXPECT_EQ(a.heap.stats().live_objects, 0u);
  EXPECT_EQ(a.heap.stats().large_frames, 0u);
}

TEST(heap_rejects_foreign_and_double_frees) {
  Arena a(1024);
  int local;
  EXPECT(!a.heap.free(&local));
  void* big = a.heap.alloc(10000);
  EXPECT(a.heap.free(big));
  EXPECT(!a.heap.free(big));
  void* small = a.heap.alloc(40);
  EXPECT(!a.heap.free(static_cast<uint8_t*>(small) + 8));  // interior pointer
  EXPECT(a.heap.free(small));
  EXPECT(a.heap.check_invariants());
}

TEST(heap_releases_empty_slabs_beyond_limit) {
  Arena a(4096);
  a.heap.set_empty_slab_limit(1);
  std::vector<void*> ptrs;
  for (int i = 0; i < 10000; i++) ptrs.push_back(a.heap.alloc(64));
  uint64_t held = a.heap.stats().slab_frames;
  EXPECT(held >= 10000 * 64 / 4096);
  for (void* p : ptrs) a.heap.free(p);
  EXPECT_EQ(a.heap.stats().slab_frames, 1u);  // one cached empty slab
  EXPECT(a.heap.check_invariants());
}
