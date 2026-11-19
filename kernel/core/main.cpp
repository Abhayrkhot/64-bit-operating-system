#include "arch/x86_64/cpu.hpp"
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/interrupts.hpp"
#include "common/mem.hpp"
#include "core/boot.hpp"
#include "core/cmdline.hpp"
#include "core/log.hpp"
#include "core/power.hpp"
#include "drivers/fw_cfg.hpp"
#include "drivers/serial.hpp"
#include "mm/kheap.hpp"
#include "mm/pmm.hpp"
#include "mm/vmm.hpp"
#include "test/ktest.hpp"

extern "C" void (*__init_array_start[])(), (*__init_array_end[])();

namespace {

void run_constructors() {
  for (auto* fn = __init_array_start; fn < __init_array_end; fn++) (*fn)();
}

void print_memory_map() {
  uint64_t usable = 0, reclaimable = 0;
  kprintf("memory map (%lu regions):\n", boot::region_count());
  for (uint64_t i = 0; i < boot::region_count(); i++) {
    const shale::MemRegion& r = boot::regions()[i];
    if (r.kind == shale::MemKind::kUsable) usable += r.pages;
    if (r.kind == shale::MemKind::kLoaderReclaimable) reclaimable += r.pages;
    kprintf("  %016lx-%016lx %8lu KiB %s\n", r.base, r.base + r.pages * shale::kPageSize, r.pages * 4,
            boot::kind_name(r.kind));
  }
  kprintf("usable %lu MiB, loader-reclaimable %lu MiB\n", usable / 256, reclaimable / 256);
}

}  // namespace

extern "C" [[noreturn]] void kmain(const shale::BootInfo* loader_info) {
  uint64_t tsc_kernel_entry = arch::rdtsc();
  serial::init();
  boot::init(loader_info);
  run_constructors();

  const shale::BootInfo& bi = boot::info();
  kprintf("\nShale kernel booting (loader->kernel %lu kcycles)\n",
          (tsc_kernel_entry - bi.tsc_loader_entry) / 1000);
  kprintf("kernel: phys %lx virt %lx size %lu KiB\n", bi.kernel_phys, bi.kernel_virt, bi.kernel_pages * 4);

  char fw_cmdline[256];
  if (fw_cfg::read_file("opt/shale/cmdline", fw_cmdline, sizeof(fw_cmdline)) >= 0)
    cmdline::init(fw_cmdline);
  else
    cmdline::init(bi.cmdline);
  kprintf("cmdline: \"%s\"\n", cmdline::raw());
  print_memory_map();

  gdt::init();
  interrupts::init();
  pmm::init();
  vmm::init();
  pmm::reclaim_boot_memory();
  kheap_init();
  kprintf("boot to ready: %lu kcycles since kernel entry\n", (arch::rdtsc() - tsc_kernel_entry) / 1000);

  char filter[64];
  if (cmdline::get("test", filter, sizeof(filter))) ktest::run(filter[0] ? filter : "all");

  kprintf("nothing to run; halting\n");
  power::qemu_exit(power::ExitCode::kSuccess);
}

// ---- boot-level tests ------------------------------------------------------

#include "fmt/format.hpp"

KTEST(boot_info_sane) {
  const shale::BootInfo& bi = boot::info();
  KEXPECT_EQ(bi.magic, shale::kBootInfoMagic);
  KEXPECT_EQ(bi.hhdm_base, shale::kHhdmBase);
  KEXPECT(bi.kernel_virt == shale::kKernelBase);
  KEXPECT(bi.region_count > 0);
  KEXPECT(bi.direct_map_top >= (4ull << 30));
  KEXPECT(bi.rsdp_phys != 0);
  bool saw_kernel = false;
  for (uint64_t i = 0; i < boot::region_count(); i++) {
    const shale::MemRegion& r = boot::regions()[i];
    if (r.kind == shale::MemKind::kKernel && r.base <= bi.kernel_phys &&
        r.base + r.pages * shale::kPageSize >= bi.kernel_phys + bi.kernel_pages * shale::kPageSize)
      saw_kernel = true;
  }
  KEXPECT(saw_kernel);
}

KTEST(boot_direct_map_reads_rsdp) {
  // The ACPI RSDP starts with "RSD PTR " and is reachable through the HHDM.
  const char* rsdp = phys_to_virt<const char>(boot::info().rsdp_phys);
  KEXPECT(memcmp(rsdp, "RSD PTR ", 8) == 0);
}

KTEST(fmt_basic) {
  char b[64];
  shale::fmt::format(b, sizeof(b), "%d|%5u|%-4x|%08lx|%s|%c", -42, 7u, 0xabu, 0x1234ul, "ok", 'z');
  KEXPECT(strcmp(b, "-42|    7|ab  |00001234|ok|z") == 0);
  int n = shale::fmt::format(b, 4, "%s", "truncated");
  KEXPECT_EQ(n, 9);
  KEXPECT(strcmp(b, "tru") == 0);
  shale::fmt::format(b, sizeof(b), "%p", reinterpret_cast<void*>(0x10));
  KEXPECT(strcmp(b, "0x0000000000000010") == 0);
}

KTEST(cmdline_parse) {
  char saved[256];
  memcpy(saved, cmdline::raw(), strlen(cmdline::raw()) + 1);
  cmdline::init("test=all iters=500 quiet name=x");
  char v[16];
  KEXPECT(cmdline::get("iters", v, sizeof(v)) && strcmp(v, "500") == 0);
  KEXPECT_EQ(cmdline::get_long("iters", 0), 500);
  KEXPECT(cmdline::has("quiet"));
  KEXPECT(!cmdline::has("qui"));
  KEXPECT(!cmdline::has("missing"));
  cmdline::init(saved);
}
