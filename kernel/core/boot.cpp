#include "core/boot.hpp"

#include "common/mem.hpp"
#include "core/log.hpp"

namespace boot {
namespace {
os::BootInfo g_info;
constexpr uint64_t kMaxRegions = 512;
os::MemRegion g_regions[kMaxRegions];
uint64_t g_region_count;
}  // namespace

void init(const os::BootInfo* loader_info) {
  if (loader_info->magic != os::kBootInfoMagic) panic("bad boot info magic %lx", loader_info->magic);
  g_info = *loader_info;
  if (g_info.region_count > kMaxRegions) panic("too many memory regions: %lu", g_info.region_count);
  memcpy(g_regions, phys_to_virt(g_info.regions_phys), g_info.region_count * sizeof(os::MemRegion));
  g_region_count = g_info.region_count;
  g_info.cmdline[sizeof(g_info.cmdline) - 1] = 0;
}

const os::BootInfo& info() { return g_info; }
const os::MemRegion* regions() { return g_regions; }
uint64_t region_count() { return g_region_count; }

const char* kind_name(os::MemKind kind) {
  switch (kind) {
    case os::MemKind::kUsable: return "usable";
    case os::MemKind::kLoaderReclaimable: return "loader-reclaimable";
    case os::MemKind::kKernel: return "kernel";
    case os::MemKind::kBootData: return "boot-data";
    case os::MemKind::kInitrd: return "initrd";
    case os::MemKind::kAcpiReclaimable: return "acpi-reclaimable";
    case os::MemKind::kAcpiNvs: return "acpi-nvs";
    case os::MemKind::kMmio: return "mmio";
    case os::MemKind::kRuntimeServices: return "uefi-runtime";
    case os::MemKind::kReserved: return "reserved";
  }
  return "?";
}

}  // namespace boot
