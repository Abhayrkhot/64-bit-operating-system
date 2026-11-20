// Boot protocol between the UEFI loader and the kernel.
//
// The loader leaves the CPU in long mode with interrupts off and its own page
// tables loaded:
//   * physical memory [0, top) is mapped twice: identity and at kHhdmBase
//     (the "higher-half direct map"), with 2 MiB pages;
//   * the kernel image is mapped at its link address (kKernelBase + offset)
//     with 4 KiB pages and per-segment permissions.
// It then jumps to the ELF entry point with RDI = HHDM address of BootInfo.
#pragma once

#include <stdint.h>

namespace os {

constexpr uint64_t kBootInfoMagic = 0x4f464e49544f4f42ull;  // "BOOTINFO"
constexpr uint64_t kHhdmBase = 0xFFFF800000000000ull;
constexpr uint64_t kKernelBase = 0xFFFFFFFF80000000ull;
constexpr uint64_t kPageSize = 4096;

// UEFI reserves memory types 0x80000000..0xFFFFFFFF for OS loaders. The
// loader tags its allocations with these so the kernel can tell them apart.
constexpr uint32_t kEfiMemKernelImage = 0x80000000u;
constexpr uint32_t kEfiMemBootData = 0x80000001u;  // BootInfo, region table, loader page tables
constexpr uint32_t kEfiMemInitrd = 0x80000002u;

enum class MemKind : uint32_t {
  kUsable = 1,             // free RAM
  kLoaderReclaimable = 2,  // loader/boot-services memory: free once the kernel is up
  kKernel = 3,             // kernel image
  kBootData = 4,           // BootInfo, region table, loader page tables
  kInitrd = 5,             // initial ramdisk (tar)
  kAcpiReclaimable = 6,
  kAcpiNvs = 7,
  kMmio = 8,
  kRuntimeServices = 9,
  kReserved = 10,
};

struct MemRegion {
  uint64_t base;   // physical, page aligned
  uint64_t pages;  // 4 KiB pages
  MemKind kind;
  uint32_t reserved;
};

enum class PixelFormat : uint32_t { kNone = 0, kRgbx = 1, kBgrx = 2, kOther = 3 };

struct Framebuffer {
  uint64_t phys;
  uint32_t width;
  uint32_t height;
  uint32_t pixels_per_line;
  PixelFormat format;
};

struct BootInfo {
  uint64_t magic;
  uint64_t kernel_phys;   // physical base of the loaded image
  uint64_t kernel_virt;   // virtual base (lowest PT_LOAD address, page aligned)
  uint64_t kernel_pages;
  uint64_t hhdm_base;
  uint64_t direct_map_top;  // physical bytes covered by the direct map
  uint64_t regions_phys;    // MemRegion[region_count], physical address
  uint64_t region_count;
  uint64_t rsdp_phys;       // ACPI RSDP, 0 if absent
  uint64_t initrd_phys;     // 0 if no initrd
  uint64_t initrd_size;
  uint64_t loader_pml4_phys;
  uint64_t tsc_loader_entry;  // rdtsc at efi_main entry
  uint64_t tsc_kernel_jump;   // rdtsc right before jumping to the kernel
  Framebuffer fb;
  char cmdline[256];
};

}  // namespace os
