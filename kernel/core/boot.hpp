// Boot information from the loader and address translation helpers.
#pragma once

#include <shale/bootinfo.hpp>
#include <stdint.h>

namespace boot {

// Copied out of loader memory on entry so it survives reclaiming that memory.
const shale::BootInfo& info();
void init(const shale::BootInfo* loader_info);
const shale::MemRegion* regions();
uint64_t region_count();
const char* kind_name(shale::MemKind kind);

}  // namespace boot

// Physical <-> kernel virtual via the higher-half direct map.
template <typename T = void>
inline T* phys_to_virt(uint64_t phys) {
  return reinterpret_cast<T*>(phys + shale::kHhdmBase);
}
inline uint64_t virt_to_phys(const void* virt) {
  auto v = reinterpret_cast<uint64_t>(virt);
  if (v >= shale::kKernelBase) return v - boot::info().kernel_virt + boot::info().kernel_phys;
  return v - shale::kHhdmBase;
}
