// In-kernel tests for the physical allocator, page tables and heap.
#include "arch/x86_64/cpu.hpp"
#include "arch/x86_64/extable.hpp"
#include "common/mem.hpp"
#include "core/boot.hpp"
#include "core/log.hpp"
#include "util/random.hpp"
#include "mm/kheap.hpp"
#include "mm/pmm.hpp"
#include "mm/vmm.hpp"
#include "test/ktest.hpp"

extern "C" char __text_start[], __rodata_start[], __data_start[];
extern "C" int probe_exec_ok();

namespace {
bool buddy_ok() {
  LockGuard g(pmm::lock());
  return pmm::buddy().check_invariants();
}
}  // namespace

KTEST(pmm_alloc_free_roundtrip) {
  uint64_t before = pmm::free_bytes();
  uint64_t pages[64];
  for (auto& p : pages) {
    p = pmm::alloc(0);
    KEXPECT(p != 0 && (p & 0xFFF) == 0);
  }
  for (int i = 0; i < 64; i++)
    for (int j = i + 1; j < 64; j++) KEXPECT(pages[i] != pages[j]);
  KEXPECT_EQ(pmm::free_bytes(), before - 64 * 4096);
  for (auto p : pages) pmm::free(p);
  KEXPECT_EQ(pmm::free_bytes(), before);
  KEXPECT(buddy_ok());
}

KTEST(pmm_blocks_are_aligned_and_coalesce) {
  int largest_before;
  {
    LockGuard g(pmm::lock());
    largest_before = pmm::buddy().largest_free_order();
  }
  for (unsigned order = 0; order <= 10; order++) {
    uint64_t p = pmm::alloc(order);
    KEXPECT(p != 0);
    KEXPECT((p & ((4096ull << order) - 1)) == 0);
    // Touch every page of the block through the direct map.
    for (uint64_t off = 0; off < (4096ull << order); off += 4096) *phys_to_virt<volatile uint64_t>(p + off) = off;
    pmm::free(p, order);
  }
  LockGuard g(pmm::lock());
  KEXPECT_EQ(pmm::buddy().largest_free_order(), largest_before);
  KEXPECT(pmm::buddy().check_invariants());
}

KTEST(pmm_random_churn_keeps_invariants) {
  os::Random rng(42);
  struct Block {
    uint64_t phys;
    unsigned order;
  } live[256] = {};
  uint64_t before = pmm::free_bytes();
  for (int step = 0; step < 20000; step++) {
    Block& b = live[rng.below(256)];
    if (b.phys) {
      pmm::free(b.phys, b.order);
      b.phys = 0;
    } else {
      b.order = static_cast<unsigned>(rng.below(6));
      b.phys = pmm::alloc(b.order);
    }
  }
  for (auto& b : live)
    if (b.phys) pmm::free(b.phys, b.order);
  KEXPECT_EQ(pmm::free_bytes(), before);
  KEXPECT(buddy_ok());
}

KTEST(vmm_kernel_text_is_read_execute) {
  KEXPECT_EQ(probe_read(__text_start), 0);
  KEXPECT_EQ(probe_write(__text_start), 1);
  KEXPECT_EQ(probe_exec(reinterpret_cast<const void*>(&probe_exec_ok)), 0);
}

KTEST(vmm_rodata_is_read_only_no_exec) {
  KEXPECT_EQ(probe_read(__rodata_start), 0);
  KEXPECT_EQ(probe_write(__rodata_start), 1);
  if (vmm::features().nx) KEXPECT_EQ(probe_exec(__rodata_start), 1);
}

KTEST(vmm_data_and_direct_map_are_no_exec) {
  if (!vmm::features().nx) return;
  static uint8_t buf[16] = {0xC3};  // `ret`, so a missing NX bit would return 0 instead of faulting
  KEXPECT_EQ(probe_write(buf), 0);
  KEXPECT_EQ(probe_exec(buf), 1);
  uint64_t p = pmm::alloc_zeroed(0);
  auto* direct = phys_to_virt<uint8_t>(p);
  direct[0] = 0xC3;
  KEXPECT_EQ(probe_exec(direct), 1);
  pmm::free(p);
}

KTEST(vmm_low_memory_is_unmapped) {
  // The loader's identity map is gone: null and low addresses fault.
  KEXPECT_EQ(probe_read(nullptr), 1);
  KEXPECT_EQ(probe_read(reinterpret_cast<void*>(0x100000)), 1);
  KEXPECT_EQ(probe_read(reinterpret_cast<void*>(boot::info().kernel_phys)), 1);
}

KTEST(vmm_map_query_unmap) {
  auto& ks = vmm::kernel_space();
  uint64_t va = vmm::kMmioRegion + (1ull << 38);  // far from real MMIO windows
  uint64_t frame = pmm::alloc_zeroed(0);
  KEXPECT(ks.map(va, frame, vmm::kPresent | vmm::kWritable | vmm::kNoExecute));
  KEXPECT(!ks.map(va, frame, vmm::kPresent));  // double map refused
  *reinterpret_cast<volatile uint64_t*>(va) = 0x1122334455667788ull;
  KEXPECT_EQ(*phys_to_virt<uint64_t>(frame), 0x1122334455667788ull);
  uint64_t pa = 0, flags = 0;
  KEXPECT(ks.query(va + 8, &pa, &flags));
  KEXPECT_EQ(pa, frame + 8);
  KEXPECT((flags & vmm::kWritable) != 0);
  KEXPECT(ks.protect(va, vmm::kPresent | vmm::kNoExecute));
  KEXPECT_EQ(probe_write(reinterpret_cast<void*>(va)), 1);
  KEXPECT(ks.unmap(va));
  KEXPECT_EQ(probe_read(reinterpret_cast<void*>(va)), 1);
  KEXPECT(!ks.query(va, nullptr, nullptr));
  pmm::free(frame);
}

KTEST(vmm_kernel_stack_has_guard_page) {
  uint64_t before = pmm::free_bytes();
  uint64_t top = vmm::alloc_kernel_stack(4);
  KEXPECT(top != 0);
  KEXPECT_EQ(probe_write(reinterpret_cast<void*>(top - 8)), 0);
  KEXPECT_EQ(probe_write(reinterpret_cast<void*>(top - 4 * 4096)), 0);
  KEXPECT_EQ(probe_write(reinterpret_cast<void*>(top - 4 * 4096 - 8)), 1);  // guard
  vmm::free_kernel_stack(top, 4);
  KEXPECT_EQ(probe_read(reinterpret_cast<void*>(top - 8)), 1);
  // Intermediate page tables stay allocated; data frames all come back.
  KEXPECT(pmm::free_bytes() >= before - 4096 * 3);
}

KTEST(vmm_user_address_space_lifecycle) {
  kfree(kmalloc(sizeof(vmm::AddressSpace)));  // warm its slab class; its empty slab stays cached
  uint64_t before = pmm::free_bytes();
  vmm::AddressSpace* as = vmm::AddressSpace::create_user();
  KEXPECT(as != nullptr);
  for (uint64_t va = 0x400000; va < 0x400000 + 16 * 4096; va += 4096)
    KEXPECT(as->map(va, pmm::alloc_zeroed(0), vmm::kPresent | vmm::kWritable | vmm::kUser | vmm::kOwned));
  KEXPECT(as->map(0x7FFFFFFFE000, pmm::alloc_zeroed(0), vmm::kPresent | vmm::kUser | vmm::kOwned));
  uint64_t stray = pmm::alloc(0);
  KEXPECT(!as->map(0x500000, stray, vmm::kPresent));  // user half needs kUser
  pmm::free(stray);
  uint64_t pa;
  KEXPECT(as->query(0x400000 + 5 * 4096, &pa, nullptr));
  // Kernel half is shared: kernel text resolves to the same frame.
  uint64_t kpa, upa;
  KEXPECT(vmm::kernel_space().query(reinterpret_cast<uint64_t>(__text_start), &kpa, nullptr));
  KEXPECT(as->query(reinterpret_cast<uint64_t>(__text_start), &upa, nullptr));
  KEXPECT_EQ(kpa, upa);
  KEXPECT(as->count_user_tables() >= 6);
  as->destroy();
  KEXPECT_EQ(pmm::free_bytes(), before);
}

KTEST(heap_size_classes_and_large) {
  static const size_t kSizes[] = {1, 16, 17, 100, 2048, 2049, 4096, 5000, 65536, 1ul << 20};
  for (size_t size : kSizes) {
    auto* p = static_cast<uint8_t*>(kmalloc(size));
    KEXPECT(p != nullptr);
    KEXPECT(kmalloc_usable_size(p) >= size);
    KEXPECT((reinterpret_cast<uint64_t>(p) & 15) == 0);
    memset(p, 0xA5, size);
    kfree(p);
  }
  int on_stack;
  KEXPECT(!kfree_checked(&on_stack));
  KEXPECT(kheap_check());
}

KTEST(heap_random_stress_and_return_to_baseline) {
  os::Random rng(7);
  constexpr int kSlots = 2048;
  static uint8_t* ptrs[kSlots];
  static uint32_t sizes[kSlots];
  // Other subsystems (the mounted file system, processes) legitimately hold
  // heap objects, so compare against a snapshot rather than zero.
  uint64_t live_before[os::mm::Heap::kNumClasses];
  for (unsigned c = 0; c < os::mm::Heap::kNumClasses; c++) live_before[c] = kheap_class_stats(c).live_objects;
  uint64_t large_before = kheap_stats().large_frames;
  for (int step = 0; step < 200000; step++) {
    int i = static_cast<int>(rng.below(kSlots));
    if (ptrs[i]) {
      for (uint32_t k = 0; k < sizes[i]; k += 61) KEXPECT(ptrs[i][k] == static_cast<uint8_t>(i + k));
      kfree(ptrs[i]);
      ptrs[i] = nullptr;
    } else {
      uint32_t size = rng.below(8) == 0 ? 1 + static_cast<uint32_t>(rng.below(20000))
                                        : 1 + static_cast<uint32_t>(rng.below(600));
      ptrs[i] = static_cast<uint8_t*>(kmalloc(size));
      sizes[i] = size;
      KEXPECT(ptrs[i] != nullptr);
      for (uint32_t k = 0; k < size; k += 61) ptrs[i][k] = static_cast<uint8_t>(i + k);
    }
  }
  KEXPECT(kheap_check());
  for (int i = 0; i < kSlots; i++) {
    kfree(ptrs[i]);
    ptrs[i] = nullptr;
  }
  KEXPECT(kheap_check());
  for (unsigned c = 0; c < os::mm::Heap::kNumClasses; c++)
    KEXPECT_EQ(kheap_class_stats(c).live_objects, live_before[c]);
  KEXPECT_EQ(kheap_stats().large_frames, large_before);
}

KTEST(heap_typed_cache) {
  static int cache = -1;
  if (cache < 0) cache = kcache_create(616);
  KEXPECT(cache >= 0);
  void* objs[100];
  for (auto& o : objs) {
    o = kcache_alloc(cache);
    KEXPECT(o != nullptr);
    KEXPECT_EQ(kmalloc_usable_size(o), 616u);
    memset(o, 0x3C, 616);
  }
  for (auto* o : objs) kfree(o);
  KEXPECT(kheap_check());
}
