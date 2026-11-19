// Virtual memory: x86-64 4-level page tables.
//
// Kernel half (PML4 slots 256-511) is created once at boot and shared by
// every address space, so kernel mappings added later are visible everywhere:
//   0xFFFF800000000000  direct map of physical memory (1 GiB or 2 MiB pages, NX)
//   0xFFFFFD0000000000  kernel stacks, one guard page below each
//   0xFFFFFE0000000000  MMIO windows (uncached)
//   0xFFFFFFFF80000000  kernel image (text R-X, rodata R--, data RW-)
// User space is [0, 0x0000800000000000).
#pragma once

#include <stdint.h>

namespace vmm {

constexpr uint64_t kPresent = 1ull << 0;
constexpr uint64_t kWritable = 1ull << 1;
constexpr uint64_t kUser = 1ull << 2;
constexpr uint64_t kWriteThrough = 1ull << 3;
constexpr uint64_t kCacheDisable = 1ull << 4;
constexpr uint64_t kAccessed = 1ull << 5;
constexpr uint64_t kDirty = 1ull << 6;
constexpr uint64_t kHuge = 1ull << 7;
constexpr uint64_t kGlobal = 1ull << 8;
constexpr uint64_t kOwned = 1ull << 9;  // software bit: frame is freed when the mapping goes
constexpr uint64_t kNoExecute = 1ull << 63;
constexpr uint64_t kAddrMask = 0x000FFFFFFFFFF000ull;

constexpr uint64_t kUserEnd = 0x0000800000000000ull;
constexpr uint64_t kStackRegion = 0xFFFFFD0000000000ull;
constexpr uint64_t kMmioRegion = 0xFFFFFE0000000000ull;

class AddressSpace {
 public:
  // Maps one 4 KiB page. Fails if va is already mapped or tables can't be allocated.
  bool map(uint64_t va, uint64_t pa, uint64_t flags);
  // Removes a 4 KiB mapping, freeing the frame if it was mapped kOwned.
  bool unmap(uint64_t va);
  // Translates va (4 KiB, 2 MiB or 1 GiB mappings). flags gets the leaf entry's bits.
  bool query(uint64_t va, uint64_t* pa, uint64_t* flags) const;
  bool protect(uint64_t va, uint64_t flags);
  void activate() const;
  bool active() const;
  uint64_t pml4_phys() const { return pml4_; }

  // A new address space sharing the kernel half; empty user half.
  static AddressSpace* create_user();
  // Frees the user half: owned frames, page tables, the PML4 and this object.
  void destroy();
  // Frames used by user-half page tables (for leak checks).
  uint64_t count_user_tables() const;

 private:
  uint64_t* leaf(uint64_t va, bool create);
  uint64_t pml4_ = 0;
  friend void init();
};

void init();
AddressSpace& kernel_space();

// Maps [phys, phys + size) uncached into the MMIO region; returns its address.
volatile void* map_mmio(uint64_t phys, uint64_t size);

// Kernel stacks: `pages` mapped pages under a guard page. Returns the stack top.
uint64_t alloc_kernel_stack(unsigned pages);
void free_kernel_stack(uint64_t top, unsigned pages);

struct Features {
  bool nx, gbpages, smep, smap, umip, pge;
};
const Features& features();

}  // namespace vmm
