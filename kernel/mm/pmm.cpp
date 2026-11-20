#include "mm/pmm.hpp"

#include "common/mem.hpp"
#include "core/boot.hpp"
#include "core/log.hpp"

namespace pmm {
namespace {

using os::MemKind;
using os::mm::BuddyAllocator;

constexpr uint64_t kFrame = os::kPageSize;
constexpr uint64_t kLowReserve = 1ull << 20;  // keep the first MiB (real-mode leftovers, SMP trampoline)

BuddyAllocator g_buddy;
SpinLock g_lock;
uint64_t g_meta_phys, g_meta_bytes;
bool g_reclaimed;

bool is_ram(MemKind k) {
  switch (k) {
    case MemKind::kUsable:
    case MemKind::kLoaderReclaimable:
    case MemKind::kKernel:
    case MemKind::kBootData:
    case MemKind::kInitrd:
    case MemKind::kAcpiReclaimable:
    case MemKind::kAcpiNvs:
    case MemKind::kRuntimeServices: return true;
    default: return false;
  }
}

// Frees [base, end) minus the low reserve and the metadata array.
void add_range(uint64_t base, uint64_t end) {
  auto add = [](uint64_t b, uint64_t e) {
    if (b < kLowReserve) b = kLowReserve;
    if (e <= b) return;
    g_buddy.add_free_range(static_cast<uint32_t>(b / kFrame), static_cast<uint32_t>((e - b) / kFrame));
  };
  uint64_t mb = g_meta_phys, me = g_meta_phys + os::align_up(g_meta_bytes, kFrame);
  if (end <= mb || base >= me) {
    add(base, end);
  } else {
    add(base, mb);
    add(me, end);
  }
}

}  // namespace

void init() {
  uint64_t top = 0;
  for (uint64_t i = 0; i < boot::region_count(); i++) {
    const os::MemRegion& r = boot::regions()[i];
    if (is_ram(r.kind)) top = os::max(top, r.base + r.pages * kFrame);
  }
  uint64_t frames = top / kFrame;
  if (frames > 0xFFFFFFF0ull) panic("pmm: %lu frames exceeds 32-bit frame index", frames);
  g_meta_bytes = frames * sizeof(os::mm::FrameMeta);
  uint64_t meta_pages = os::align_up(g_meta_bytes, kFrame) / kFrame;

  // Metadata goes at the start of the first usable region above 1 MiB that fits.
  for (uint64_t i = 0; i < boot::region_count() && !g_meta_phys; i++) {
    const os::MemRegion& r = boot::regions()[i];
    uint64_t base = os::max(r.base, kLowReserve);
    uint64_t end = r.base + r.pages * kFrame;
    if (r.kind == MemKind::kUsable && end > base && (end - base) / kFrame >= meta_pages) g_meta_phys = base;
  }
  if (!g_meta_phys) panic("pmm: no room for %lu KiB of frame metadata", g_meta_bytes / 1024);

  g_buddy.init(phys_to_virt<os::mm::FrameMeta>(g_meta_phys), static_cast<uint32_t>(frames));
  for (uint64_t i = 0; i < boot::region_count(); i++) {
    const os::MemRegion& r = boot::regions()[i];
    if (r.kind == MemKind::kUsable) add_range(r.base, r.base + r.pages * kFrame);
  }
  kprintf("pmm: %lu frames (%lu MiB of RAM), %lu MiB free, metadata %lu KiB at %lx\n", frames, top >> 20,
          free_bytes() >> 20, g_meta_bytes / 1024, g_meta_phys);
}

void reclaim_boot_memory() {
  LockGuard guard(g_lock);
  if (g_reclaimed) return;
  g_reclaimed = true;
  uint64_t before = g_buddy.free_frames();
  for (uint64_t i = 0; i < boot::region_count(); i++) {
    const os::MemRegion& r = boot::regions()[i];
    if (r.kind == MemKind::kLoaderReclaimable || r.kind == MemKind::kBootData)
      add_range(r.base, r.base + r.pages * kFrame);
  }
  kprintf("pmm: reclaimed %lu KiB of loader memory\n", (g_buddy.free_frames() - before) * 4);
}

uint64_t alloc(unsigned order) {
  LockGuard guard(g_lock);
  uint32_t f = g_buddy.alloc(order);
  return f == BuddyAllocator::kNone ? 0 : static_cast<uint64_t>(f) * kFrame;
}

uint64_t alloc_zeroed(unsigned order) {
  uint64_t p = alloc(order);
  if (p) memset(phys_to_virt(p), 0, kFrame << order);
  return p;
}

void free(uint64_t phys, unsigned order) {
  LockGuard guard(g_lock);
  if (!g_buddy.free(static_cast<uint32_t>(phys / kFrame), order))
    panic("pmm: bad free of %lx (order %u)", phys, order);
}

uint64_t free_bytes() { return g_buddy.free_frames() * kFrame; }
uint64_t managed_bytes() { return g_buddy.managed_frames() * kFrame; }
uint64_t metadata_bytes() { return g_meta_bytes; }
os::mm::BuddyAllocator& buddy() { return g_buddy; }
SpinLock& lock() { return g_lock; }

}  // namespace pmm
