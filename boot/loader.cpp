// Shale UEFI loader.
//
// Loads \kernel.elf (and optionally \initrd.tar and \cmdline.txt) from the
// volume it was started from, builds page tables for the kernel, exits boot
// services and jumps to the kernel. See include/shale/bootinfo.hpp for the
// hand-off contract.
#include <shale/bootinfo.hpp>
#include <shale/elf.hpp>

#include "efi.hpp"

using namespace efi;

extern "C" {
void* memset(void* dst, int c, size_t n) {
  auto* d = static_cast<uint8_t*>(dst);
  for (size_t i = 0; i < n; i++) d[i] = static_cast<uint8_t>(c);
  return dst;
}
void* memcpy(void* dst, const void* src, size_t n) {
  auto* d = static_cast<uint8_t*>(dst);
  auto* s = static_cast<const uint8_t*>(src);
  for (size_t i = 0; i < n; i++) d[i] = s[i];
  return dst;
}
}

namespace {

constexpr uint64_t kPresent = 1ull << 0;
constexpr uint64_t kWritable = 1ull << 1;
constexpr uint64_t kHuge = 1ull << 7;
constexpr uint64_t kNoExecute = 1ull << 63;
constexpr uint64_t kAddrMask = 0x000FFFFFFFFFF000ull;
constexpr uint64_t kGiB = 1ull << 30;
constexpr uint32_t kMsrEfer = 0xC0000080;
constexpr uint64_t kEferNxe = 1ull << 11;

SystemTable* g_st;
BootServices* g_bs;

inline uint64_t rdtsc() {
  uint32_t lo, hi;
  asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return (static_cast<uint64_t>(hi) << 32) | lo;
}

void print(const char* s) {
  Char16 buf[130];
  size_t n = 0;
  while (*s) {
    if (*s == '\n') buf[n++] = u'\r';
    buf[n++] = static_cast<Char16>(*s++);
    if (n >= 126) {
      buf[n] = 0;
      g_st->con_out->output_string(g_st->con_out, buf);
      n = 0;
    }
  }
  buf[n] = 0;
  g_st->con_out->output_string(g_st->con_out, buf);
}

void print_hex(uint64_t v) {
  char b[19] = {'0', 'x'};
  for (int i = 0; i < 16; i++) {
    int d = (v >> (60 - 4 * i)) & 0xf;
    b[2 + i] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
  }
  b[18] = 0;
  print(b);
}

void print_dec(uint64_t v) {
  char b[21];
  int i = 20;
  b[i] = 0;
  do {
    b[--i] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v);
  print(b + i);
}

[[noreturn]] void fail(const char* what, Status st) {
  print("shale-boot: error: ");
  print(what);
  print(" (status ");
  print_hex(st);
  print(")\n");
  for (;;) g_bs->stall(1000000);
}

void check(Status st, const char* what) {
  if (st != kSuccess) fail(what, st);
}

uint64_t alloc_pages(uint64_t pages, uint32_t type) {
  PhysAddr addr = 0;
  check(g_bs->allocate_pages(kAllocateAnyPages, type, pages, &addr), "AllocatePages");
  memset(reinterpret_cast<void*>(addr), 0, pages * shale::kPageSize);
  return addr;
}

struct FileData {
  uint8_t* data = nullptr;
  uint64_t size = 0;
};

// Reads a whole file into freshly allocated pages of the given memory type.
bool read_file(File* root, const Char16* path, uint32_t mem_type, FileData* out) {
  File* f = nullptr;
  if (root->open(root, &f, path, kFileModeRead, 0) != kSuccess) return false;
  uint64_t size = 0;
  check(f->set_position(f, ~0ull), "SetPosition(end)");
  check(f->get_position(f, &size), "GetPosition");
  check(f->set_position(f, 0), "SetPosition(0)");
  uint64_t pages = (size + shale::kPageSize - 1) / shale::kPageSize;
  uint64_t addr = alloc_pages(pages ? pages : 1, mem_type);
  UINTN n = size;
  check(f->read(f, &n, reinterpret_cast<void*>(addr)), "Read");
  if (n != size) fail("short read", n);
  f->close(f);
  out->data = reinterpret_cast<uint8_t*>(addr);
  out->size = size;
  return true;
}

uint64_t* table_at(uint64_t entry) { return reinterpret_cast<uint64_t*>(entry & kAddrMask); }

uint64_t* next_table(uint64_t* table, unsigned index) {
  if (!(table[index] & kPresent))
    table[index] = alloc_pages(1, shale::kEfiMemBootData) | kPresent | kWritable;
  return table_at(table[index]);
}

void map_page(uint64_t* pml4, uint64_t va, uint64_t pa, uint64_t flags) {
  uint64_t* pdpt = next_table(pml4, (va >> 39) & 511);
  uint64_t* pd = next_table(pdpt, (va >> 30) & 511);
  uint64_t* pt = next_table(pd, (va >> 21) & 511);
  pt[(va >> 12) & 511] = (pa & kAddrMask) | flags;
}

struct KernelImage {
  uint64_t phys = 0;
  uint64_t virt = 0;
  uint64_t pages = 0;
  uint64_t entry = 0;
};

// Validates the kernel ELF and copies its PT_LOAD segments into one physically
// contiguous allocation.
KernelImage load_kernel(const FileData& file) {
  if (file.size < sizeof(shale::elf::Ehdr)) fail("kernel.elf too small", file.size);
  const auto& eh = *reinterpret_cast<const shale::elf::Ehdr*>(file.data);
  if (!shale::elf::has_valid_ident(eh) || eh.type != shale::elf::kTypeExec ||
      eh.machine != shale::elf::kMachineX8664 || eh.phentsize != sizeof(shale::elf::Phdr))
    fail("kernel.elf is not an x86-64 ELF64 executable", 0);
  if (eh.phoff > file.size || eh.phnum * sizeof(shale::elf::Phdr) > file.size - eh.phoff)
    fail("kernel.elf program headers out of bounds", eh.phoff);
  const auto* ph = reinterpret_cast<const shale::elf::Phdr*>(file.data + eh.phoff);

  uint64_t lo = ~0ull, hi = 0;
  for (unsigned i = 0; i < eh.phnum; i++) {
    if (ph[i].type != shale::elf::kPtLoad || ph[i].memsz == 0) continue;
    if (ph[i].vaddr < shale::kKernelBase || ph[i].filesz > ph[i].memsz ||
        ph[i].offset > file.size || ph[i].filesz > file.size - ph[i].offset)
      fail("bad kernel segment", i);
    uint64_t start = ph[i].vaddr & ~(shale::kPageSize - 1);
    uint64_t end = (ph[i].vaddr + ph[i].memsz + shale::kPageSize - 1) & ~(shale::kPageSize - 1);
    if (start < lo) lo = start;
    if (end > hi) hi = end;
  }
  if (hi <= lo) fail("kernel has no loadable segments", 0);

  KernelImage k;
  k.virt = lo;
  k.pages = (hi - lo) / shale::kPageSize;
  k.phys = alloc_pages(k.pages, shale::kEfiMemKernelImage);  // zeroed: covers .bss
  k.entry = eh.entry;
  for (unsigned i = 0; i < eh.phnum; i++) {
    if (ph[i].type != shale::elf::kPtLoad || ph[i].memsz == 0) continue;
    memcpy(reinterpret_cast<void*>(k.phys + (ph[i].vaddr - lo)), file.data + ph[i].offset, ph[i].filesz);
  }
  return k;
}

bool cpu_has_nx() {
  uint32_t a, b, c, d;
  asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001u), "c"(0));
  return d & (1u << 20);
}

// Builds the hand-off page tables: identity + HHDM for [0, top) and the
// kernel image at its link address.
uint64_t* build_page_tables(const KernelImage& k, const uint8_t* elf_file, uint64_t top, bool nx) {
  auto* pml4 = reinterpret_cast<uint64_t*>(alloc_pages(1, shale::kEfiMemBootData));
  for (uint64_t gb = 0; gb < top / kGiB; gb++) {
    uint64_t* pdpt = next_table(pml4, static_cast<unsigned>(gb / 512));
    pml4[256 + gb / 512] = pml4[gb / 512];  // HHDM shares the identity PDPTs
    auto* pd = reinterpret_cast<uint64_t*>(alloc_pages(1, shale::kEfiMemBootData));
    pdpt[gb % 512] = reinterpret_cast<uint64_t>(pd) | kPresent | kWritable;
    for (uint64_t i = 0; i < 512; i++) pd[i] = (gb * kGiB + (i << 21)) | kPresent | kWritable | kHuge;
  }

  const auto& eh = *reinterpret_cast<const shale::elf::Ehdr*>(elf_file);
  const auto* ph = reinterpret_cast<const shale::elf::Phdr*>(elf_file + eh.phoff);
  for (unsigned i = 0; i < eh.phnum; i++) {
    if (ph[i].type != shale::elf::kPtLoad || ph[i].memsz == 0) continue;
    uint64_t flags = kPresent;
    if (ph[i].flags & shale::elf::kPfW) flags |= kWritable;
    if (!(ph[i].flags & shale::elf::kPfX) && nx) flags |= kNoExecute;
    uint64_t start = ph[i].vaddr & ~(shale::kPageSize - 1);
    uint64_t end = (ph[i].vaddr + ph[i].memsz + shale::kPageSize - 1) & ~(shale::kPageSize - 1);
    for (uint64_t va = start; va < end; va += shale::kPageSize) map_page(pml4, va, k.phys + (va - k.virt), flags);
  }
  return pml4;
}

shale::MemKind classify(uint32_t type) {
  switch (type) {
    case kConventionalMemory: return shale::MemKind::kUsable;
    case kLoaderCode:
    case kLoaderData:
    case kBootServicesCode:
    case kBootServicesData: return shale::MemKind::kLoaderReclaimable;
    case kAcpiReclaimMemory: return shale::MemKind::kAcpiReclaimable;
    case kAcpiMemoryNvs: return shale::MemKind::kAcpiNvs;
    case kMemoryMappedIo:
    case kMemoryMappedIoPortSpace: return shale::MemKind::kMmio;
    case kRuntimeServicesCode:
    case kRuntimeServicesData: return shale::MemKind::kRuntimeServices;
    case shale::kEfiMemKernelImage: return shale::MemKind::kKernel;
    case shale::kEfiMemBootData: return shale::MemKind::kBootData;
    case shale::kEfiMemInitrd: return shale::MemKind::kInitrd;
    default: return shale::MemKind::kReserved;
  }
}

const MemoryDescriptor* desc_at(const uint8_t* map, UINTN desc_size, UINTN i) {
  return reinterpret_cast<const MemoryDescriptor*>(map + i * desc_size);
}

// Highest physical address the direct map must cover: all RAM-like memory and
// at least 4 GiB (so the LAPIC/IOAPIC and 32-bit PCI windows are reachable).
uint64_t direct_map_top(const uint8_t* map, UINTN map_size, UINTN desc_size) {
  uint64_t top = 4 * kGiB;
  for (UINTN i = 0; i < map_size / desc_size; i++) {
    const auto* d = desc_at(map, desc_size, i);
    shale::MemKind kind = classify(d->type);
    if (kind == shale::MemKind::kMmio || kind == shale::MemKind::kReserved) continue;
    uint64_t end = d->physical_start + d->number_of_pages * shale::kPageSize;
    if (end > top) top = end;
  }
  return (top + kGiB - 1) & ~(kGiB - 1);
}

}  // namespace

extern "C" Status efi_main(Handle image, SystemTable* st) {
  uint64_t tsc_entry = rdtsc();
  g_st = st;
  g_bs = st->boot_services;
  g_bs->set_watchdog_timer(0, 0, 0, nullptr);
  print("shale-boot: loader started\n");

  LoadedImage* loaded = nullptr;
  check(g_bs->handle_protocol(image, &kLoadedImageGuid, reinterpret_cast<void**>(&loaded)), "LoadedImage");
  SimpleFileSystem* fs = nullptr;
  check(g_bs->handle_protocol(loaded->device_handle, &kSimpleFileSystemGuid, reinterpret_cast<void**>(&fs)),
        "SimpleFileSystem");
  File* root = nullptr;
  check(fs->open_volume(fs, &root), "OpenVolume");

  FileData kernel_file;
  if (!read_file(root, u"\\kernel.elf", kLoaderData, &kernel_file)) fail("\\kernel.elf not found", kNotFound);
  KernelImage kernel = load_kernel(kernel_file);
  print("shale-boot: kernel ");
  print_dec(kernel.pages * 4);
  print(" KiB at phys ");
  print_hex(kernel.phys);
  print("\n");

  auto* bi = reinterpret_cast<shale::BootInfo*>(alloc_pages(1, shale::kEfiMemBootData));
  bi->magic = shale::kBootInfoMagic;
  bi->kernel_phys = kernel.phys;
  bi->kernel_virt = kernel.virt;
  bi->kernel_pages = kernel.pages;
  bi->hhdm_base = shale::kHhdmBase;
  bi->tsc_loader_entry = tsc_entry;

  FileData initrd;
  if (read_file(root, u"\\initrd.tar", shale::kEfiMemInitrd, &initrd)) {
    bi->initrd_phys = reinterpret_cast<uint64_t>(initrd.data);
    bi->initrd_size = initrd.size;
    print("shale-boot: initrd ");
    print_dec(initrd.size);
    print(" bytes\n");
  }
  FileData cmdline;
  if (read_file(root, u"\\cmdline.txt", kLoaderData, &cmdline)) {
    uint64_t n = 0;
    for (; n < cmdline.size && n < sizeof(bi->cmdline) - 1; n++) {
      char c = static_cast<char>(cmdline.data[n]);
      if (c == '\n' || c == '\r') break;
      bi->cmdline[n] = c;
    }
  }

  for (UINTN i = 0; i < st->number_of_table_entries; i++) {
    const auto& t = st->configuration_table[i];
    if (t.vendor_guid == kAcpi20TableGuid) bi->rsdp_phys = reinterpret_cast<uint64_t>(t.vendor_table);
  }
  if (!bi->rsdp_phys) {
    for (UINTN i = 0; i < st->number_of_table_entries; i++) {
      const auto& t = st->configuration_table[i];
      if (t.vendor_guid == kAcpi10TableGuid) bi->rsdp_phys = reinterpret_cast<uint64_t>(t.vendor_table);
    }
  }

  GraphicsOutput* gop = nullptr;
  if (g_bs->locate_protocol(&kGraphicsOutputGuid, nullptr, reinterpret_cast<void**>(&gop)) == kSuccess && gop &&
      gop->mode && gop->mode->info) {
    const GopModeInfo* info = gop->mode->info;
    bi->fb.phys = gop->mode->frame_buffer_base;
    bi->fb.width = info->horizontal_resolution;
    bi->fb.height = info->vertical_resolution;
    bi->fb.pixels_per_line = info->pixels_per_scan_line;
    bi->fb.format = info->pixel_format == kPixelRgbx8   ? shale::PixelFormat::kRgbx
                    : info->pixel_format == kPixelBgrx8 ? shale::PixelFormat::kBgrx
                                                        : shale::PixelFormat::kOther;
  }

  // Size the memory map, then allocate the buffers the final map and region
  // table will live in. Slack covers descriptors added by these allocations.
  UINTN map_size = 0, map_key = 0, desc_size = 0;
  uint32_t desc_version = 0;
  Status s = g_bs->get_memory_map(&map_size, nullptr, &map_key, &desc_size, &desc_version);
  if (s != kBufferTooSmall) fail("GetMemoryMap(size)", s);
  UINTN map_capacity = map_size + 64 * desc_size;
  uint64_t map_pages = (map_capacity + shale::kPageSize - 1) / shale::kPageSize;
  auto* map = reinterpret_cast<uint8_t*>(alloc_pages(map_pages, shale::kEfiMemBootData));
  uint64_t region_pages = (map_capacity / desc_size * sizeof(shale::MemRegion)) / shale::kPageSize + 1;
  auto* regions = reinterpret_cast<shale::MemRegion*>(alloc_pages(region_pages, shale::kEfiMemBootData));

  map_size = map_capacity;
  check(g_bs->get_memory_map(&map_size, reinterpret_cast<MemoryDescriptor*>(map), &map_key, &desc_size,
                             &desc_version),
        "GetMemoryMap");
  bi->direct_map_top = direct_map_top(map, map_size, desc_size);
  bool nx = cpu_has_nx();
  uint64_t* pml4 = build_page_tables(kernel, kernel_file.data, bi->direct_map_top, nx);
  bi->loader_pml4_phys = reinterpret_cast<uint64_t>(pml4);
  print("shale-boot: direct map ");
  print_dec(bi->direct_map_top / kGiB);
  print(" GiB, exiting boot services\n");

  // After the first ExitBootServices attempt only GetMemoryMap and
  // ExitBootServices may be called, so the retry loop allocates nothing.
  for (int attempt = 0;; attempt++) {
    map_size = map_capacity;
    s = g_bs->get_memory_map(&map_size, reinterpret_cast<MemoryDescriptor*>(map), &map_key, &desc_size,
                             &desc_version);
    if (s != kSuccess) fail("GetMemoryMap(final)", s);
    s = g_bs->exit_boot_services(image, map_key);
    if (s == kSuccess) break;
    if (attempt == 8) fail("ExitBootServices", s);
  }

  // Boot services are gone: no more printing through ConOut.
  uint64_t count = 0;
  for (UINTN i = 0; i < map_size / desc_size; i++) {
    const auto* d = desc_at(map, desc_size, i);
    shale::MemKind kind = classify(d->type);
    if (count && regions[count - 1].kind == kind &&
        regions[count - 1].base + regions[count - 1].pages * shale::kPageSize == d->physical_start) {
      regions[count - 1].pages += d->number_of_pages;
      continue;
    }
    regions[count++] = {d->physical_start, d->number_of_pages, kind, 0};
  }
  bi->regions_phys = reinterpret_cast<uint64_t>(regions);
  bi->region_count = count;

  if (nx) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(kMsrEfer));
    uint64_t efer = ((static_cast<uint64_t>(hi) << 32) | lo) | kEferNxe;
    asm volatile("wrmsr" ::"c"(kMsrEfer), "a"(static_cast<uint32_t>(efer)), "d"(static_cast<uint32_t>(efer >> 32)));
  }
  bi->tsc_kernel_jump = rdtsc();
  uint64_t bi_virt = reinterpret_cast<uint64_t>(bi) + shale::kHhdmBase;
  asm volatile(
      "cli\n\t"
      "mov %0, %%cr3\n\t"
      "jmp *%1\n\t" ::"r"(pml4),
      "r"(kernel.entry), "D"(bi_virt)
      : "memory");
  __builtin_unreachable();
}
