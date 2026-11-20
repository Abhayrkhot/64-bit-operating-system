// Boot information from the loader and address translation helpers.
#pragma once

#include <abi/bootinfo.hpp>
#include <stdint.h>

namespace boot {

// Copied out of loader memory on entry so it survives reclaiming that memory.
const os::BootInfo& info();
void init(const os::BootInfo* loader_info);
const os::MemRegion* regions();
uint64_t region_count();
const char* kind_name(os::MemKind kind);

}  // namespace boot

// Physical <-> kernel virtual via the higher-half direct map.
template <typename T = void>
inline T* phys_to_virt(uint64_t phys) {
  return reinterpret_cast<T*>(phys + os::kHhdmBase);
}
inline uint64_t virt_to_phys(const void* virt) {
  auto v = reinterpret_cast<uint64_t>(virt);
  if (v >= os::kKernelBase) return v - boot::info().kernel_virt + boot::info().kernel_phys;
  return v - os::kHhdmBase;
}
