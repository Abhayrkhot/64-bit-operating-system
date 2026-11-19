#include "mm/vmm.hpp"

#include "arch/x86_64/cpu.hpp"
#include "common/mem.hpp"
#include "core/boot.hpp"
#include "core/log.hpp"
#include "mm/kheap.hpp"
#include "mm/pmm.hpp"
#include "sync/spinlock.hpp"

extern "C" char __text_start[], __text_end[], __rodata_start[], __rodata_end[], __data_start[], __kernel_end[];

namespace vmm {
namespace {

constexpr uint64_t kPage = 4096;
constexpr uint64_t kTableFlags = kPresent | kWritable;
constexpr uint64_t kStackSlotSize = 64 * 1024;  // VA per stack slot (max 15 pages + guard)
constexpr unsigned kStackSlots = 4096;

AddressSpace g_kernel;
Features g_features;
SpinLock g_lock;  // protects kernel-half table edits and the region allocators
uint64_t g_mmio_next = kMmioRegion;
uint64_t g_stack_bitmap[kStackSlots / 64];

uint64_t* table(uint64_t entry) { return phys_to_virt<uint64_t>(entry & kAddrMask); }
unsigned index(uint64_t va, int level) { return (va >> (12 + 9 * level)) & 511; }

void flush(const AddressSpace& as, uint64_t va) {
  if (va >= kUserEnd || as.active()) arch::invlpg(va);
}

}  // namespace

const Features& features() { return g_features; }
AddressSpace& kernel_space() { return g_kernel; }

uint64_t* AddressSpace::leaf(uint64_t va, bool create) {
  uint64_t* t = phys_to_virt<uint64_t>(pml4_);
  for (int level = 3; level >= 1; level--) {
    uint64_t& e = t[index(va, level)];
    if (!(e & kPresent)) {
      if (!create) return nullptr;
      uint64_t phys = pmm::alloc_zeroed(0);
      if (!phys) return nullptr;
      e = phys | kTableFlags | (va < kUserEnd ? kUser : 0);
    } else if (e & kHuge) {
      return nullptr;  // covered by a large page; 4 KiB operations don't apply
    }
    t = table(e);
  }
  return &t[index(va, 0)];
}

bool AddressSpace::map(uint64_t va, uint64_t pa, uint64_t flags) {
  if ((va | pa) & (kPage - 1)) return false;
  if ((va < kUserEnd) != ((flags & kUser) != 0)) return false;  // user bit must match the half
  LockGuard guard(g_lock);
  uint64_t* e = leaf(va, true);
  if (!e || (*e & kPresent)) return false;
  if (!g_features.nx) flags &= ~kNoExecute;
  *e = (pa & kAddrMask) | flags | kPresent;
  return true;
}

bool AddressSpace::unmap(uint64_t va) {
  uint64_t old;
  {
    LockGuard guard(g_lock);
    uint64_t* e = leaf(va, false);
    if (!e || !(*e & kPresent)) return false;
    old = *e;
    *e = 0;
    flush(*this, va);
  }
  if (old & kOwned) pmm::free(old & kAddrMask);
  return true;
}

bool AddressSpace::protect(uint64_t va, uint64_t flags) {
  LockGuard guard(g_lock);
  uint64_t* e = leaf(va, false);
  if (!e || !(*e & kPresent)) return false;
  if (!g_features.nx) flags &= ~kNoExecute;
  *e = (*e & (kAddrMask | kOwned)) | flags | kPresent;
  flush(*this, va);
  return true;
}

bool AddressSpace::query(uint64_t va, uint64_t* pa, uint64_t* flags) const {
  const uint64_t* t = phys_to_virt<uint64_t>(pml4_);
  for (int level = 3; level >= 0; level--) {
    uint64_t e = t[index(va, level)];
    if (!(e & kPresent)) return false;
    if (level == 0 || (level <= 2 && (e & kHuge))) {
      uint64_t span = 1ull << (12 + 9 * level);
      if (pa) *pa = (e & kAddrMask & ~(span - 1)) + (va & (span - 1));
      if (flags) *flags = e & ~kAddrMask;
      return true;
    }
    t = table(e);
  }
  return false;
}

void AddressSpace::activate() const { arch::write_cr3(pml4_); }
bool AddressSpace::active() const { return (arch::read_cr3() & kAddrMask) == pml4_; }

AddressSpace* AddressSpace::create_user() {
  auto* as = static_cast<AddressSpace*>(kzalloc(sizeof(AddressSpace)));
  if (!as) return nullptr;
  as->pml4_ = pmm::alloc_zeroed(0);
  if (!as->pml4_) {
    kfree(as);
    return nullptr;
  }
  uint64_t* dst = phys_to_virt<uint64_t>(as->pml4_);
  const uint64_t* src = phys_to_virt<uint64_t>(g_kernel.pml4_);
  for (int i = 256; i < 512; i++) dst[i] = src[i];
  return as;
}

void AddressSpace::destroy() {
  KASSERT(this != &g_kernel);
  KASSERT(!active());
  uint64_t* l4 = phys_to_virt<uint64_t>(pml4_);
  for (int i4 = 0; i4 < 256; i4++) {
    if (!(l4[i4] & kPresent)) continue;
    uint64_t* l3 = table(l4[i4]);
    for (int i3 = 0; i3 < 512; i3++) {
      if (!(l3[i3] & kPresent)) continue;
      uint64_t* l2 = table(l3[i3]);
      for (int i2 = 0; i2 < 512; i2++) {
        if (!(l2[i2] & kPresent)) continue;
        uint64_t* l1 = table(l2[i2]);
        for (int i1 = 0; i1 < 512; i1++)
          if ((l1[i1] & kPresent) && (l1[i1] & kOwned)) pmm::free(l1[i1] & kAddrMask);
        pmm::free(l2[i2] & kAddrMask);
      }
      pmm::free(l3[i3] & kAddrMask);
    }
    pmm::free(l4[i4] & kAddrMask);
  }
  pmm::free(pml4_);
  kfree(this);
}

uint64_t AddressSpace::count_user_tables() const {
  uint64_t n = 0;
  const uint64_t* l4 = phys_to_virt<uint64_t>(pml4_);
  for (int i4 = 0; i4 < 256; i4++) {
    if (!(l4[i4] & kPresent)) continue;
    n++;
    const uint64_t* l3 = table(l4[i4]);
    for (int i3 = 0; i3 < 512; i3++) {
      if (!(l3[i3] & kPresent)) continue;
      n++;
      const uint64_t* l2 = table(l3[i3]);
      for (int i2 = 0; i2 < 512; i2++) n += (l2[i2] & kPresent) ? 1 : 0;
    }
  }
  return n;
}

void init() {
  auto ext = arch::cpuid(0x80000001);
  auto leaf7 = arch::cpuid(7, 0);
  auto leaf1 = arch::cpuid(1);
  g_features.nx = ext.edx & (1u << 20);
  g_features.gbpages = ext.edx & (1u << 26);
  g_features.pge = leaf1.edx & (1u << 13);
  g_features.smep = leaf7.ebx & (1u << 7);
  g_features.smap = leaf7.ebx & (1u << 20);
  g_features.umip = leaf7.ecx & (1u << 2);
  uint64_t nx = g_features.nx ? kNoExecute : 0;
  uint64_t global = g_features.pge ? kGlobal : 0;

  g_kernel.pml4_ = pmm::alloc_zeroed(0);
  uint64_t* l4 = phys_to_virt<uint64_t>(g_kernel.pml4_);
  for (int i = 256; i < 512; i++) {
    uint64_t t = pmm::alloc_zeroed(0);
    if (!t) panic("vmm: out of memory for kernel page tables");
    l4[i] = t | kTableFlags;
  }

  // Direct map.
  uint64_t top = boot::info().direct_map_top;
  uint64_t direct_flags = kPresent | kWritable | kHuge | global | nx;
  for (uint64_t pa = 0; pa < top; pa += 1ull << 30) {
    uint64_t va = shale::kHhdmBase + pa;
    uint64_t* l3 = table(l4[index(va, 3)]);
    if (g_features.gbpages) {
      l3[index(va, 2)] = pa | direct_flags;
      continue;
    }
    uint64_t pd = pmm::alloc_zeroed(0);
    if (!pd) panic("vmm: out of memory for the direct map");
    l3[index(va, 2)] = pd | kTableFlags;
    uint64_t* l2 = phys_to_virt<uint64_t>(pd);
    for (unsigned i = 0; i < 512; i++) l2[i] = (pa + (static_cast<uint64_t>(i) << 21)) | direct_flags;
  }

  // Kernel image with W^X.
  struct Range {
    const char* start;
    const char* end;
    uint64_t flags;
  } ranges[] = {
      {__text_start, __text_end, kPresent | global},
      {__rodata_start, __rodata_end, kPresent | global | nx},
      {__data_start, __kernel_end, kPresent | kWritable | global | nx},
  };
  const shale::BootInfo& bi = boot::info();
  for (const Range& r : ranges) {
    for (uint64_t va = reinterpret_cast<uint64_t>(r.start); va < reinterpret_cast<uint64_t>(r.end); va += kPage) {
      if (!g_kernel.map(va, bi.kernel_phys + (va - bi.kernel_virt), r.flags))
        panic("vmm: failed to map kernel page %lx", va);
    }
  }

  uint64_t efer = arch::rdmsr(arch::kMsrEfer);
  if (g_features.nx) arch::wrmsr(arch::kMsrEfer, efer | (1ull << 11));
  g_kernel.activate();
  arch::write_cr0(arch::read_cr0() | (1ull << 16));  // WP: ring 0 honours read-only pages
  uint64_t cr4 = arch::read_cr4();
  if (g_features.pge) cr4 |= 1ull << 7;
  if (g_features.umip) cr4 |= 1ull << 11;
  if (g_features.smep) cr4 |= 1ull << 20;
  if (g_features.smap) cr4 |= 1ull << 21;
  arch::write_cr4(cr4);
  kprintf("vmm: kernel page tables live (direct map %lu GiB via %s pages; nx=%d smep=%d smap=%d umip=%d)\n",
          top >> 30, g_features.gbpages ? "1 GiB" : "2 MiB", g_features.nx, g_features.smep, g_features.smap,
          g_features.umip);
}

volatile void* map_mmio(uint64_t phys, uint64_t size) {
  uint64_t base = phys & ~(kPage - 1);
  uint64_t pages = (phys + size - base + kPage - 1) / kPage;
  uint64_t va;
  {
    LockGuard guard(g_lock);
    va = g_mmio_next;
    g_mmio_next += (pages + 1) * kPage;  // leave a hole between windows
  }
  for (uint64_t i = 0; i < pages; i++) {
    if (!g_kernel.map(va + i * kPage, base + i * kPage,
                      kPresent | kWritable | kCacheDisable | kWriteThrough | kNoExecute | kGlobal))
      panic("vmm: map_mmio failed");
  }
  return reinterpret_cast<volatile void*>(va + (phys - base));
}

uint64_t alloc_kernel_stack(unsigned pages) {
  KASSERT(pages >= 1 && pages * kPage < kStackSlotSize);
  unsigned slot = kStackSlots;
  {
    LockGuard guard(g_lock);
    for (unsigned i = 0; i < kStackSlots; i++) {
      if (!(g_stack_bitmap[i / 64] & (1ull << (i % 64)))) {
        g_stack_bitmap[i / 64] |= 1ull << (i % 64);
        slot = i;
        break;
      }
    }
  }
  if (slot == kStackSlots) return 0;
  uint64_t top = kStackRegion + (slot + 1) * kStackSlotSize;
  for (unsigned i = 0; i < pages; i++) {
    uint64_t frame = pmm::alloc(0);
    if (!frame || !g_kernel.map(top - (i + 1) * kPage, frame, kPresent | kWritable | kNoExecute | kGlobal | kOwned)) {
      if (frame) pmm::free(frame);
      free_kernel_stack(top, i);
      return 0;
    }
  }
  return top;  // the page below the lowest mapped page stays unmapped: guard
}

void free_kernel_stack(uint64_t top, unsigned pages) {
  for (unsigned i = 0; i < pages; i++) g_kernel.unmap(top - (i + 1) * kPage);
  unsigned slot = static_cast<unsigned>((top - kStackRegion) / kStackSlotSize) - 1;
  LockGuard guard(g_lock);
  g_stack_bitmap[slot / 64] &= ~(1ull << (slot % 64));
}

}  // namespace vmm
