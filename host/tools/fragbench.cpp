// fragbench: trace-driven fragmentation comparison of Shale's allocators
// against textbook baselines. All allocators replay the same seeded trace.
//
// Page level (1 GiB of 4 KiB frames, power-of-two requests, ~80% occupancy):
//   buddy      Shale's BuddyAllocator (lib/mm/buddy.cpp, the kernel's code)
//   first-fit  address-ordered free-extent list
//   next-fit   bitmap with a roving pointer (common in hobby kernels)
// Metric: share of free memory that sits in naturally aligned fully free
// 64 KiB / 2 MiB blocks (what contiguous and huge-page requests can use), and
// requests that failed although enough total memory was free.
//
// Heap level (kernel-object mix, ~42k live objects, ~16 MB live):
//   slab       Shale's Heap (lib/mm/heap.cpp) over the buddy allocator
//   first-fit  address-ordered first fit, boundary tags, 8-byte headers
//   best-fit   best fit, same block format
// Metric: overhead = 1 - live_bytes / footprint (footprint = memory taken
// from the page allocator / arena top), in steady state and after a shrink.
//
//   fragbench [--seed N] [--quick] [--csv FILE]
#include <sys/mman.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include "mm/buddy.hpp"
#include "mm/heap.hpp"
#include "util/random.hpp"

using shale::Random;
using shale::mm::BuddyAllocator;
using shale::mm::FrameMeta;

namespace {

uint64_t exp_sample(Random& rng, double mean) {
  double u = (static_cast<double>(rng.next() >> 11) + 0.5) / 9007199254740992.0;
  return static_cast<uint64_t>(-std::log(u) * mean) + 1;
}

// ---------------------------------------------------------------------------
// Page level

constexpr uint32_t kFrames = 1u << 18;  // 1 GiB

struct PageAlloc {
  virtual ~PageAlloc() = default;
  virtual const char* name() const = 0;
  virtual int64_t alloc(uint32_t pages) = 0;
  virtual void free(uint32_t frame, uint32_t pages) = 0;
};

struct BuddyPages : PageAlloc {
  std::vector<FrameMeta> meta{kFrames};
  BuddyAllocator b;
  BuddyPages() {
    b.init(meta.data(), kFrames);
    b.add_free_range(0, kFrames);
  }
  const char* name() const override { return "buddy (Shale)"; }
  int64_t alloc(uint32_t pages) override {
    uint32_t f = b.alloc_exact(pages);
    return f == BuddyAllocator::kNone ? -1 : f;
  }
  void free(uint32_t frame, uint32_t pages) override { b.free_exact(frame, pages); }
};

struct FirstFitPages : PageAlloc {
  std::map<uint32_t, uint32_t> extents{{0, kFrames}};  // start -> length
  const char* name() const override { return "first-fit extents"; }
  int64_t alloc(uint32_t pages) override {
    for (auto it = extents.begin(); it != extents.end(); ++it) {
      if (it->second < pages) continue;
      uint32_t start = it->first, len = it->second;
      extents.erase(it);
      if (len > pages) extents.emplace(start + pages, len - pages);
      return start;
    }
    return -1;
  }
  void free(uint32_t frame, uint32_t pages) override {
    auto it = extents.emplace(frame, pages).first;
    auto next = std::next(it);
    if (next != extents.end() && it->first + it->second == next->first) {
      it->second += next->second;
      extents.erase(next);
    }
    if (it != extents.begin()) {
      auto prev = std::prev(it);
      if (prev->first + prev->second == it->first) {
        prev->second += it->second;
        extents.erase(it);
      }
    }
  }
};

struct NextFitBitmap : PageAlloc {
  std::vector<uint8_t> used = std::vector<uint8_t>(kFrames, 0);
  uint32_t rover = 0;
  const char* name() const override { return "next-fit bitmap"; }
  int64_t alloc(uint32_t pages) override {
    uint32_t scanned = 0, run = 0, pos = rover;
    while (scanned < kFrames + pages) {
      if (pos == kFrames) {
        pos = 0;
        run = 0;
      }
      run = used[pos] ? 0 : run + 1;
      pos++;
      scanned++;
      if (run == pages) {
        uint32_t start = pos - pages;
        std::memset(&used[start], 1, pages);
        rover = pos;
        return start;
      }
    }
    return -1;
  }
  void free(uint32_t frame, uint32_t pages) override { std::memset(&used[frame], 0, pages); }
};

struct PageResult {
  std::string name;
  double util = 0, aligned64k = 0, aligned2m = 0;
  uint64_t requests = 0, failures = 0, frag_failures = 0, frag_failures_2m = 0;
};

PageResult run_pages(PageAlloc& a, uint64_t seed, uint64_t warmup, uint64_t ticks) {
  static const uint32_t kSizes[] = {1, 2, 4, 8, 16, 64, 512};
  static const uint32_t kWeights[] = {55, 15, 12, 8, 6, 3, 1};
  Random rng(seed);
  std::vector<uint8_t> shadow(kFrames, 0);
  uint64_t free_frames = kFrames;
  using Death = std::pair<uint64_t, std::pair<uint32_t, uint32_t>>;  // tick, (frame, pages)
  std::priority_queue<Death, std::vector<Death>, std::greater<Death>> deaths;
  PageResult r;
  r.name = a.name();
  uint64_t samples = 0;
  for (uint64_t t = 0; t < warmup + ticks; t++) {
    while (!deaths.empty() && deaths.top().first <= t) {
      auto [frame, pages] = deaths.top().second;
      deaths.pop();
      a.free(frame, pages);
      std::memset(&shadow[frame], 0, pages);
      free_frames += pages;
    }
    uint32_t w = static_cast<uint32_t>(rng.below(100)), size = kSizes[0];
    for (int i = 0, acc = 0; i < 7; i++) {
      acc += kWeights[i];
      if (w < static_cast<uint32_t>(acc)) {
        size = kSizes[i];
        break;
      }
    }
    uint64_t life = rng.chance(15, 100) ? exp_sample(rng, 128000) : exp_sample(rng, 2000);
    bool measuring = t >= warmup;
    int64_t frame = a.alloc(size);
    if (measuring) r.requests++;
    if (frame < 0) {
      if (measuring) {
        r.failures++;
        if (free_frames >= size) r.frag_failures++;
        if (free_frames >= size && size == 512) r.frag_failures_2m++;
      }
      continue;
    }
    for (uint32_t i = 0; i < size; i++) {
      if (shadow[frame + i]) {
        std::fprintf(stderr, "%s: overlapping allocation at frame %ld\n", a.name(), static_cast<long>(frame));
        std::exit(1);
      }
      shadow[frame + i] = 1;
    }
    free_frames -= size;
    deaths.push({t + life, {static_cast<uint32_t>(frame), size}});
    if (measuring && t % 20000 == 0) {
      uint64_t in64k = 0, in2m = 0;
      for (uint32_t b = 0; b < kFrames; b += 16) {
        bool all_free = std::all_of(&shadow[b], &shadow[b] + 16, [](uint8_t v) { return v == 0; });
        if (all_free) in64k += 16;
      }
      for (uint32_t b = 0; b < kFrames; b += 512) {
        bool all_free = std::all_of(&shadow[b], &shadow[b] + 512, [](uint8_t v) { return v == 0; });
        if (all_free) in2m += 512;
      }
      r.util += 1.0 - static_cast<double>(free_frames) / kFrames;
      r.aligned64k += free_frames ? static_cast<double>(in64k) / free_frames : 0;
      r.aligned2m += free_frames ? static_cast<double>(in2m) / free_frames : 0;
      samples++;
    }
  }
  r.util /= samples;
  r.aligned64k /= samples;
  r.aligned2m /= samples;
  return r;
}

// ---------------------------------------------------------------------------
// Heap level

constexpr uint64_t kArenaBytes = 1ull << 30;

uint8_t* map_arena() {
  void* p = mmap(nullptr, kArenaBytes + (4 << 20), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                 -1, 0);
  if (p == MAP_FAILED) {
    std::perror("mmap");
    std::exit(1);
  }
  auto addr = reinterpret_cast<uintptr_t>(p);
  return reinterpret_cast<uint8_t*>((addr + (4 << 20) - 1) & ~static_cast<uintptr_t>((4 << 20) - 1));
}

struct HeapAlloc {
  virtual ~HeapAlloc() = default;
  virtual const char* name() const = 0;
  virtual void* alloc(size_t size) = 0;
  // Allocation of a known object type; heaps without typed caches use alloc().
  virtual void* alloc_typed(unsigned type, size_t size) { return alloc(size); }
  virtual void free(void* p) = 0;
  virtual uint64_t footprint() const = 0;
};

struct ObjType {
  const char* name;
  uint32_t min_size, max_size;
  uint32_t weight;
  double mean_life;
};

// A kernel-like object mix: long-lived caches (inodes, dentries, tasks)
// interleaved with short-lived buffers, which is what fragments a heap.
const ObjType kTypes[] = {
    {"vm_area", 136, 136, 12, 20000},  {"inode", 616, 616, 6, 200000},  {"dentry", 192, 192, 14, 150000},
    {"file", 232, 232, 10, 2000},      {"buffer_head", 104, 104, 10, 30000}, {"name", 16, 256, 20, 300},
    {"skb_data", 256, 2048, 10, 1000}, {"pipe_page", 4096, 4096, 3, 5000}, {"task", 1408, 1408, 2, 100000},
    {"list_node", 40, 40, 13, 10000},
};
constexpr unsigned kNumTypes = sizeof(kTypes) / sizeof(kTypes[0]);

struct SlabHeap : HeapAlloc {
  uint8_t* arena = map_arena();
  std::vector<FrameMeta> meta{kArenaBytes / 4096};
  BuddyAllocator buddy;
  shale::mm::Heap heap;
  bool typed;
  int cache_of[kNumTypes];
  explicit SlabHeap(bool use_typed_caches) : typed(use_typed_caches) {
    buddy.init(meta.data(), static_cast<uint32_t>(meta.size()));
    buddy.add_free_range(1, static_cast<uint32_t>(meta.size() - 1));
    heap.init(&buddy, arena);
    for (unsigned i = 0; i < kNumTypes; i++) {
      const ObjType& t = kTypes[i];
      cache_of[i] = typed && t.min_size == t.max_size && t.min_size <= 4096 - 64 ? heap.create_cache(t.min_size) : -1;
    }
  }
  const char* name() const override { return typed ? "slab (Shale, typed caches)" : "slab (Shale, kmalloc only)"; }
  void* alloc(size_t size) override { return heap.alloc(size); }
  void* alloc_typed(unsigned type, size_t size) override {
    return cache_of[type] >= 0 ? heap.alloc_from(cache_of[type]) : heap.alloc(size);
  }
  void free(void* p) override {
    if (!heap.free(p)) {
      std::fprintf(stderr, "slab: bad free\n");
      std::exit(1);
    }
  }
  uint64_t footprint() const override { return heap.footprint_bytes(); }
};

// Classic boundary-tag heap (dlmalloc-style block format): 8-byte header
// holding size | inuse | prev_inuse, footer on free blocks, 16-byte aligned
// payloads, coalescing on free, trimming of a free block at the top.
class FitHeap : public HeapAlloc {
 public:
  explicit FitHeap(bool best) : best_(best), tree_(2 * kChunks, 0) {}
  const char* name() const override { return best_ ? "best-fit" : "first-fit (addr-ordered)"; }

  void* alloc(size_t size) override {
    uint64_t need = std::max<uint64_t>(32, (size + 8 + 15) & ~15ull);
    int64_t off = find(need);
    if (off < 0) {
      // Grow at the top. A free block never touches the top (free() trims
      // it), so the block below is in use, or there is none.
      off = static_cast<int64_t>(top_);
      if (top_ + need > kArenaBytes) return nullptr;
      top_ += need;
      hdr(off) = need | 1 | 2;
      return arena_ + off + 8;
    }
    remove_free(off);
    uint64_t bsize = hdr(off) & ~15ull;
    uint64_t prev_inuse = hdr(off) & 2;
    if (bsize - need >= 32) {
      uint64_t rem = off + need;
      hdr(rem) = (bsize - need) | 2;  // free, prev in use
      ftr(rem) = bsize - need;
      insert_free(rem);
      hdr(off) = need | 1 | prev_inuse;
    } else {
      hdr(off) = bsize | 1 | prev_inuse;
      if (off + bsize < top_) hdr(off + bsize) |= 2;
    }
    return arena_ + off + 8;
  }

  void free(void* p) override {
    uint64_t off = static_cast<uint8_t*>(p) - arena_ - 8;
    uint64_t size = hdr(off) & ~15ull;
    uint64_t prev_inuse = hdr(off) & 2;
    uint64_t next = off + size;
    if (next < top_ && !(hdr(next) & 1)) {
      remove_free(next);
      size += hdr(next) & ~15ull;
    }
    if (!prev_inuse) {
      uint64_t psize = *reinterpret_cast<uint64_t*>(arena_ + off - 8);
      off -= psize;
      remove_free(off);
      size += psize;
      prev_inuse = hdr(off) & 2;
    }
    if (off + size == top_) {  // trim
      top_ = off;
      return;
    }
    hdr(off) = size | prev_inuse;
    ftr(off) = size;
    hdr(off + size) &= ~2ull;
    insert_free(off);
  }

  uint64_t footprint() const override { return (top_ + 4095) & ~4095ull; }

 private:
  static constexpr uint64_t kStart = 8;  // payloads land 16-byte aligned
  static constexpr uint64_t kChunkShift = 16;
  static constexpr uint64_t kChunks = kArenaBytes >> kChunkShift;

  uint64_t& hdr(uint64_t off) { return *reinterpret_cast<uint64_t*>(arena_ + off); }
  uint64_t& ftr(uint64_t off) { return *reinterpret_cast<uint64_t*>(arena_ + off + (hdr(off) & ~15ull) - 8); }

  void insert_free(uint64_t off) {
    uint64_t size = hdr(off) & ~15ull;
    if (best_) {
      by_size_.insert({size, off});
    } else {
      by_addr_.emplace(off, size);
      update_chunk(off >> kChunkShift);
    }
  }
  void remove_free(uint64_t off) {
    uint64_t size = hdr(off) & ~15ull;
    if (best_) {
      by_size_.erase({size, off});
    } else {
      by_addr_.erase(off);
      update_chunk(off >> kChunkShift);
    }
  }
  // Segment tree over 64 KiB chunks holding the largest free block starting
  // in each, so address-ordered first fit is O(log n) instead of a list scan.
  void update_chunk(uint64_t c) {
    uint64_t best = 0;
    for (auto it = by_addr_.lower_bound(c << kChunkShift); it != by_addr_.end() && (it->first >> kChunkShift) == c; ++it)
      best = std::max(best, it->second);
    uint64_t i = c + kChunks;
    tree_[i] = best;
    for (i >>= 1; i; i >>= 1) tree_[i] = std::max(tree_[2 * i], tree_[2 * i + 1]);
  }
  int64_t find(uint64_t need) {
    if (best_) {
      auto it = by_size_.lower_bound({need, 0});
      return it == by_size_.end() ? -1 : static_cast<int64_t>(it->second);
    }
    if (tree_[1] < need) return -1;
    uint64_t i = 1;
    while (i < kChunks) i = tree_[2 * i] >= need ? 2 * i : 2 * i + 1;
    uint64_t c = i - kChunks;
    for (auto it = by_addr_.lower_bound(c << kChunkShift); it != by_addr_.end(); ++it)
      if (it->second >= need) return static_cast<int64_t>(it->first);
    return -1;
  }

  bool best_;
  uint8_t* arena_ = map_arena();
  uint64_t top_ = kStart;
  std::map<uint64_t, uint64_t> by_addr_;
  std::set<std::pair<uint64_t, uint64_t>> by_size_;
  std::vector<uint64_t> tree_;
};

struct HeapResult {
  std::string name;
  double steady_overhead = 0;
  uint64_t peak_footprint = 0, peak_live = 0;
  double shrink_overhead = 0;
  uint64_t shrink_footprint = 0, shrink_live = 0;
};

HeapResult run_heap(HeapAlloc& h, uint64_t seed, uint64_t warmup, uint64_t ticks) {
  Random rng(seed);
  struct Obj {
    void* p;
    uint32_t size;
  };
  std::vector<Obj> objs;
  std::vector<uint32_t> free_slots;
  using Death = std::pair<uint64_t, uint32_t>;
  std::priority_queue<Death, std::vector<Death>, std::greater<Death>> deaths;
  uint64_t live = 0, samples = 0;
  uint32_t total_weight = 0;
  for (const ObjType& t : kTypes) total_weight += t.weight;
  HeapResult r;
  r.name = h.name();
  auto release = [&](uint32_t id) {
    h.free(objs[id].p);
    live -= objs[id].size;
    objs[id].p = nullptr;
    free_slots.push_back(id);
  };
  for (uint64_t t = 0; t < warmup + ticks; t++) {
    while (!deaths.empty() && deaths.top().first <= t) {
      uint32_t id = deaths.top().second;
      deaths.pop();
      if (objs[id].p) release(id);
    }
    uint32_t w = static_cast<uint32_t>(rng.below(total_weight));
    unsigned type_index = 0;
    for (unsigned i = 0; i < kNumTypes; i++) {
      if (w < kTypes[i].weight) {
        type_index = i;
        break;
      }
      w -= kTypes[i].weight;
    }
    const ObjType* type = &kTypes[type_index];
    uint32_t size = type->min_size + static_cast<uint32_t>(rng.below(type->max_size - type->min_size + 1));
    uint64_t life = exp_sample(rng, type->mean_life);
    void* p = h.alloc_typed(type_index, size);
    if (!p) {
      std::fprintf(stderr, "%s: out of memory\n", h.name());
      std::exit(1);
    }
    std::memset(p, 0x5A, std::min<uint32_t>(size, 64));
    uint32_t id;
    if (!free_slots.empty()) {
      id = free_slots.back();
      free_slots.pop_back();
      objs[id] = {p, size};
    } else {
      id = static_cast<uint32_t>(objs.size());
      objs.push_back({p, size});
    }
    live += size;
    deaths.push({t + life, id});
    if (t >= warmup && t % 10000 == 0) {
      uint64_t fp = h.footprint();
      r.steady_overhead += 1.0 - static_cast<double>(live) / fp;
      if (fp > r.peak_footprint) {
        r.peak_footprint = fp;
        r.peak_live = live;
      }
      samples++;
    }
  }
  r.steady_overhead /= samples;
  // Shrink: drop a random 90% of everything live (a cache flush under memory
  // pressure) and see how much memory each allocator can give back.
  Random shrink_rng(seed ^ 0xABCDEF);
  for (uint32_t id = 0; id < objs.size(); id++)
    if (objs[id].p && shrink_rng.chance(9, 10)) release(id);
  r.shrink_live = live;
  r.shrink_footprint = h.footprint();
  r.shrink_overhead = 1.0 - static_cast<double>(live) / r.shrink_footprint;
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t seed = 1;
  bool quick = false;
  const char* csv = nullptr;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
    else if (!std::strcmp(argv[i], "--quick")) quick = true;
    else if (!std::strcmp(argv[i], "--csv") && i + 1 < argc) csv = argv[++i];
    else {
      std::fprintf(stderr, "usage: fragbench [--seed N] [--quick] [--csv FILE]\n");
      return 2;
    }
  }
  uint64_t page_warmup = quick ? 200000 : 600000, page_ticks = quick ? 400000 : 3000000;
  uint64_t heap_warmup = quick ? 400000 : 1000000, heap_ticks = quick ? 600000 : 4000000;

  std::printf("fragbench seed=%lu %s\n\n", static_cast<unsigned long>(seed), quick ? "(quick)" : "");
  std::printf("Page allocators: 1 GiB, %lu requests measured after %lu warm-up\n",
              static_cast<unsigned long>(page_ticks), static_cast<unsigned long>(page_warmup));
  std::printf("%-20s %6s %14s %14s %10s %12s\n", "allocator", "util", "free in 64K", "free in 2M", "failures",
              "2M frag-fail");
  std::vector<PageResult> pages;
  {
    BuddyPages a;
    pages.push_back(run_pages(a, seed, page_warmup, page_ticks));
  }
  {
    FirstFitPages a;
    pages.push_back(run_pages(a, seed, page_warmup, page_ticks));
  }
  {
    NextFitBitmap a;
    pages.push_back(run_pages(a, seed, page_warmup, page_ticks));
  }
  for (const PageResult& r : pages)
    std::printf("%-20s %5.1f%% %13.1f%% %13.1f%% %10lu %12lu\n", r.name.c_str(), 100 * r.util, 100 * r.aligned64k,
                100 * r.aligned2m, static_cast<unsigned long>(r.frag_failures),
                static_cast<unsigned long>(r.frag_failures_2m));

  std::printf("\nHeaps: kernel-object mix, %lu allocations measured after %lu warm-up\n",
              static_cast<unsigned long>(heap_ticks), static_cast<unsigned long>(heap_warmup));
  std::printf("%-28s %10s %12s %12s %14s %12s\n", "allocator", "overhead", "peak MiB", "peak live", "after shrink",
              "shrunk MiB");
  std::vector<HeapResult> heaps;
  {
    SlabHeap h(true);
    heaps.push_back(run_heap(h, seed, heap_warmup, heap_ticks));
  }
  {
    SlabHeap h(false);
    heaps.push_back(run_heap(h, seed, heap_warmup, heap_ticks));
  }
  {
    FitHeap h(false);
    heaps.push_back(run_heap(h, seed, heap_warmup, heap_ticks));
  }
  {
    FitHeap h(true);
    heaps.push_back(run_heap(h, seed, heap_warmup, heap_ticks));
  }
  for (const HeapResult& r : heaps)
    std::printf("%-28s %9.1f%% %12.1f %12.1f %13.1f%% %12.1f\n", r.name.c_str(), 100 * r.steady_overhead,
                r.peak_footprint / 1048576.0, r.peak_live / 1048576.0, 100 * r.shrink_overhead,
                r.shrink_footprint / 1048576.0);

  if (csv) {
    FILE* f = std::fopen(csv, "w");
    std::fprintf(f, "level,allocator,seed,util,free_in_64k,free_in_2m,frag_failures,frag_failures_2m,"
                    "steady_overhead,peak_footprint,peak_live,shrink_overhead,shrink_footprint,shrink_live\n");
    for (const PageResult& r : pages)
      std::fprintf(f, "page,%s,%lu,%.4f,%.4f,%.4f,%lu,%lu,,,,,,\n", r.name.c_str(), static_cast<unsigned long>(seed),
                   r.util, r.aligned64k, r.aligned2m, static_cast<unsigned long>(r.frag_failures),
                   static_cast<unsigned long>(r.frag_failures_2m));
    for (const HeapResult& r : heaps)
      std::fprintf(f, "heap,%s,%lu,,,,,,%.4f,%lu,%lu,%.4f,%lu,%lu\n", r.name.c_str(),
                   static_cast<unsigned long>(seed), r.steady_overhead, static_cast<unsigned long>(r.peak_footprint),
                   static_cast<unsigned long>(r.peak_live), r.shrink_overhead,
                   static_cast<unsigned long>(r.shrink_footprint), static_cast<unsigned long>(r.shrink_live));
    std::fclose(f);
  }
  return 0;
}
