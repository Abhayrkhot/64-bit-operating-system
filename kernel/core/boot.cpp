#include "core/boot.hpp"

#include "common/mem.hpp"
#include "core/log.hpp"

namespace boot {
namespace {
shale::BootInfo g_info;
constexpr uint64_t kMaxRegions = 512;
shale::MemRegion g_regions[kMaxRegions];
uint64_t g_region_count;
}  // namespace

void init(const shale::BootInfo* loader_info) {
  if (loader_info->magic != shale::kBootInfoMagic) panic("bad boot info magic %lx", loader_info->magic);
  g_info = *loader_info;
  if (g_info.region_count > kMaxRegions) panic("too many memory regions: %lu", g_info.region_count);
  memcpy(g_regions, phys_to_virt(g_info.regions_phys), g_info.region_count * sizeof(shale::MemRegion));
  g_region_count = g_info.region_count;
  g_info.cmdline[sizeof(g_info.cmdline) - 1] = 0;
}

const shale::BootInfo& info() { return g_info; }
const shale::MemRegion* regions() { return g_regions; }
uint64_t region_count() { return g_region_count; }

const char* kind_name(shale::MemKind kind) {
  switch (kind) {
    case shale::MemKind::kUsable: return "usable";
    case shale::MemKind::kLoaderReclaimable: return "loader-reclaimable";
    case shale::MemKind::kKernel: return "kernel";
    case shale::MemKind::kBootData: return "boot-data";
    case shale::MemKind::kInitrd: return "initrd";
    case shale::MemKind::kAcpiReclaimable: return "acpi-reclaimable";
    case shale::MemKind::kAcpiNvs: return "acpi-nvs";
    case shale::MemKind::kMmio: return "mmio";
    case shale::MemKind::kRuntimeServices: return "uefi-runtime";
    case shale::MemKind::kReserved: return "reserved";
  }
  return "?";
}

}  // namespace boot
