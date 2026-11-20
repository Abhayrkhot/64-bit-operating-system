#include "proc/process.hpp"

#include <abi/syscall.hpp>

#include "arch/x86_64/cpu.hpp"
#include "arch/x86_64/fpu.hpp"
#include "arch/x86_64/interrupts.hpp"
#include "common/mem.hpp"
#include "core/boot.hpp"
#include "core/log.hpp"
#include "drivers/console.hpp"
#include "elf/validate.hpp"
#include "fs/vfs.hpp"
#include "mm/kheap.hpp"
#include "mm/pmm.hpp"
#include "mm/vmm.hpp"

extern "C" [[noreturn]] void enter_user(uint64_t rip, uint64_t rsp, uint64_t argc, uint64_t argv);

namespace process {
namespace {

using namespace os::sys;
constexpr uint64_t kPage = 4096;
constexpr uint64_t kProgramLo = 0x10000;              // keep the first 64 KiB unmapped
constexpr uint64_t kProgramHi = 0x0000010000000000ull;  // programs + brk live below 1 TiB
constexpr uint64_t kBrkMax = 256ull << 20;
constexpr uint64_t kUserPage = vmm::kPresent | vmm::kUser | vmm::kOwned;

Process g_kernel_proc;
Process* g_all;
int g_next_pid = 1;
int g_proc_cache = -1;
uint64_t g_count;
Process* g_orphans;  // auto-reap zombies waiting for the reaper (linked by sibling)
WaitQueue g_reaper_wq;

void reap(Process* p) {
  sched::join(p->thread);  // waits until the thread is off its stack, frees it
  p->as->destroy();
  bool irq = arch::irq_save();
  for (Process** q = &g_all; *q; q = &(*q)->all_next) {
    if (*q == p) {
      *q = p->all_next;
      break;
    }
  }
  g_count--;
  arch::irq_restore(irq);
  kfree(p);
}

void reaper(void*) {
  for (;;) {
    bool irq = arch::irq_save();
    while (!g_orphans) g_reaper_wq.wait();
    Process* p = g_orphans;
    g_orphans = p->sibling;
    arch::irq_restore(irq);
    reap(p);
  }
}

void unlink_child(Process* parent, Process* child) {
  for (Process** q = &parent->first_child; *q; q = &(*q)->sibling) {
    if (*q == child) {
      *q = child->sibling;
      child->sibling = nullptr;
      return;
    }
  }
}

// Maps one zeroed user page; returns false on OOM or conflict.
bool map_zeroed(vmm::AddressSpace* as, uint64_t va, uint64_t flags, uint64_t* frame_out = nullptr) {
  uint64_t frame = pmm::alloc_zeroed(0);
  if (!frame) return false;
  if (!as->map(va, frame, flags)) {
    pmm::free(frame);
    return false;
  }
  if (frame_out) *frame_out = frame;
  return true;
}

int load_image(Process* p, const uint8_t* data, uint64_t size) {
  os::elf::Image img;
  os::elf::Error err = os::elf::validate(data, size, kProgramLo, kProgramHi, &img);
  if (err != os::elf::Error::kOk) {
    kprintf("exec %s: %s\n", p->name, os::elf::error_name(err));
    return -kENOEXEC;
  }
  for (unsigned i = 0; i < img.count; i++) {
    const os::elf::Segment& s = img.segments[i];
    uint64_t flags = kUserPage | ((s.flags & os::elf::kPfW) ? vmm::kWritable : 0) |
                     ((s.flags & os::elf::kPfX) ? 0 : vmm::kNoExecute);
    for (uint64_t va = os::align_down(s.vaddr, kPage); va < s.vaddr + s.memsz; va += kPage) {
      uint64_t frame;
      if (!map_zeroed(p->as, va, flags, &frame)) return -kENOMEM;
      // Copy the part of this page that the file backs.
      uint64_t lo = os::max(va, s.vaddr), hi = os::min(va + kPage, s.vaddr + s.filesz);
      if (lo < hi) memcpy(phys_to_virt<uint8_t>(frame) + (lo - va), data + s.offset + (lo - s.vaddr), hi - lo);
    }
  }
  p->entry = img.entry;
  p->brk_start = p->brk_end = img.highest;
  return 0;
}

// Stack plus argv: strings and the pointer array go in the top stack page.
int setup_stack(Process* p, const char* const* argv) {
  for (uint64_t i = 1; i <= kStackPages; i++)
    if (!map_zeroed(p->as, kStackTop - i * kPage, kUserPage | vmm::kWritable | vmm::kNoExecute)) return -kENOMEM;
  p->pages_mapped += kStackPages;
  uint64_t top_frame;
  if (!p->as->query(kStackTop - kPage, &top_frame, nullptr)) return -kENOMEM;
  uint8_t* page = phys_to_virt<uint8_t>(top_frame);
  uint64_t page_va = kStackTop - kPage;
  uint64_t argc = 0;
  while (argv && argv[argc]) argc++;
  if (argc > kMaxArgs) return -kE2BIG;
  uint64_t ptrs[kMaxArgs + 1];
  uint64_t sp = kStackTop;
  for (uint64_t i = 0; i < argc; i++) {
    uint64_t len = strlen(argv[i]) + 1;
    if (sp - page_va < len + (argc + 2) * 8 + 64) return -kE2BIG;
    sp -= len;
    memcpy(page + (sp - page_va), argv[i], len);
    ptrs[i] = sp;
  }
  ptrs[argc] = 0;
  sp &= ~7ull;
  sp -= (argc + 1) * 8;
  memcpy(page + (sp - page_va), ptrs, (argc + 1) * 8);
  p->argv = sp;
  p->argc = argc;
  p->user_stack = sp & ~15ull;
  return 0;
}

void user_thread_entry(void* arg) {
  auto* p = static_cast<Process*>(arg);
  enter_user(p->entry, p->user_stack, p->argc, p->argv);
}

int signal_for(uint64_t vector) {
  switch (vector) {
    case 0:
    case 16:
    case 19: return kSigFpe;
    case 6: return kSigIll;
    default: return kSigSegv;
  }
}

void on_user_fault(interrupts::Frame* f, const char* what) {
  Process* p = current();
  if (!p) panic("user-mode fault without a process");
  if (f->vector == interrupts::kVectorPageFault)
    kprintf("pid %d (%s): %s at %016lx, rip %016lx: killed\n", p->pid, p->name, what, arch::read_cr2(), f->rip);
  else
    kprintf("pid %d (%s): %s at rip %016lx: killed\n", p->pid, p->name, what, f->rip);
  arch::sti();
  exit(128 + signal_for(f->vector));
}

void on_return_to_user(interrupts::Frame*) {
  Process* p = current();
  if (p && p->killed) {
    arch::sti();
    exit(128 + kSigKill);
  }
}

}  // namespace

void init() {
  g_proc_cache = kcache_create(sizeof(Process));
  g_kernel_proc.pid = 0;
  memcpy(g_kernel_proc.name, "kernel", 7);
  interrupts::set_user_fault_hook(on_user_fault);
  interrupts::set_return_to_user_hook(on_return_to_user);
  sched::spawn("reaper", reaper, nullptr);
}

Process* kernel() { return &g_kernel_proc; }
Process* current() { return sched::current()->process; }
uint64_t count() { return g_count; }

int spawn(const char* path, const char* const* argv, Process* parent) {
  const uint8_t* image;
  uint64_t size;
  int r = vfs::load_executable(path, &image, &size);
  if (r < 0) return r;
  auto* p = static_cast<Process*>(kcache_alloc(g_proc_cache));
  if (!p) return -kENOMEM;
  memset(p, 0, sizeof(*p));
  new (&p->child_exited) WaitQueue();
  const char* base = path;
  for (const char* c = path; *c; c++)
    if (*c == '/') base = c + 1;
  size_t n = os::min(strlen(base), sizeof(p->name) - 1);
  memcpy(p->name, base, n);
  p->mmap_next = kMmapBase;
  p->as = vmm::AddressSpace::create_user();
  r = p->as ? load_image(p, image, size) : -kENOMEM;
  if (r == 0) r = setup_stack(p, argv);
  vfs::release_executable(image);
  Thread* t = nullptr;
  FpuState* fpu = nullptr;
  if (r == 0) {
    t = sched::create(p->name, user_thread_entry, p);
    fpu = static_cast<FpuState*>(kmalloc(sizeof(FpuState)));
    if (!t || !fpu) r = -kENOMEM;
  }
  if (r < 0) {
    if (t) sched::discard(t);
    kfree(fpu);
    if (p->as) p->as->destroy();
    kfree(p);
    return r;
  }
  fpu::init_state(fpu);
  t->fpu = fpu;
  t->user_fpu = true;
  t->address_space = p->as;
  t->process = p;
  p->thread = t;
  // Standard streams: inherited from a user parent, the console otherwise.
  for (int fd = 0; fd < 3; fd++) {
    File* f = parent && parent != &g_kernel_proc ? parent->fds[fd] : nullptr;
    p->fds[fd] = f ? file_get(f) : console::open();
  }
  bool irq = arch::irq_save();
  p->pid = g_next_pid++;
  p->parent = parent;
  p->sibling = parent->first_child;
  parent->first_child = p;
  p->all_next = g_all;
  g_all = p;
  g_count++;
  int pid = p->pid;
  arch::irq_restore(irq);
  sched::make_runnable(t);
  return pid;
}

int wait(Process* parent, int pid, int* status) {
  for (;;) {
    bool irq = arch::irq_save();
    bool any = false;
    Process* zombie = nullptr;
    for (Process* c = parent->first_child; c; c = c->sibling) {
      if (pid != -1 && c->pid != pid) continue;
      any = true;
      if (c->state == ProcState::kZombie) {
        zombie = c;
        break;
      }
    }
    if (!any) {
      arch::irq_restore(irq);
      return -kECHILD;
    }
    if (zombie) {
      unlink_child(parent, zombie);
      arch::irq_restore(irq);
      int zpid = zombie->pid;
      if (status) *status = zombie->exit_status;
      reap(zombie);
      return zpid;
    }
    parent->child_exited.wait();
    arch::irq_restore(irq);
  }
}

void exit(int status) {
  Process* p = current();
  KASSERT(p != nullptr);
  for (int fd = 0; fd < kMaxFds; fd++) close_fd(p, fd);
  arch::cli();
  // Orphans go to the kernel and are reaped automatically.
  while (Process* c = p->first_child) {
    p->first_child = c->sibling;
    c->parent = &g_kernel_proc;
    c->auto_reap = true;
    if (c->state == ProcState::kZombie) {
      c->sibling = g_orphans;
      g_orphans = c;
      g_reaper_wq.wake_one();
    } else {
      c->sibling = g_kernel_proc.first_child;
      g_kernel_proc.first_child = c;
    }
  }
  p->exit_status = status;
  p->state = ProcState::kZombie;
  if (p->auto_reap) {
    unlink_child(p->parent, p);
    p->sibling = g_orphans;
    g_orphans = p;
    g_reaper_wq.wake_one();
  } else {
    p->parent->child_exited.wake_all();
  }
  sched::exit(status);
}

int kill(Process* caller, int pid) {
  bool irq = arch::irq_save();
  for (Process* c = caller->first_child; c; c = c->sibling) {
    if (c->pid == pid) {
      c->killed = true;
      arch::irq_restore(irq);
      return 0;
    }
  }
  arch::irq_restore(irq);
  return -kESRCH;
}

int install_fd(Process* p, File* f) {
  for (int fd = 0; fd < kMaxFds; fd++) {
    if (!p->fds[fd]) {
      p->fds[fd] = f;
      return fd;
    }
  }
  return -kEMFILE;
}

File* get_fd(Process* p, int fd) {
  if (fd < 0 || fd >= kMaxFds) return nullptr;
  return p->fds[fd];
}

int close_fd(Process* p, int fd) {
  File* f = get_fd(p, fd);
  if (!f) return -kEBADF;
  p->fds[fd] = nullptr;
  file_put(f);
  return 0;
}

uint64_t brk(Process* p, uint64_t new_end) {
  if (new_end == 0 || new_end < p->brk_start || new_end - p->brk_start > kBrkMax) return p->brk_end;
  uint64_t old_top = os::align_up(p->brk_end, kPage), new_top = os::align_up(new_end, kPage);
  if (new_top > old_top) {
    uint64_t pages = (new_top - old_top) / kPage;
    if (p->pages_mapped + pages > kPageLimit) return p->brk_end;
    for (uint64_t va = old_top; va < new_top; va += kPage) {
      if (!map_zeroed(p->as, va, kUserPage | vmm::kWritable | vmm::kNoExecute)) {
        for (uint64_t undo = old_top; undo < va; undo += kPage) p->as->unmap(undo);
        return p->brk_end;
      }
    }
    p->pages_mapped += pages;
  } else {
    for (uint64_t va = new_top; va < old_top; va += kPage) p->as->unmap(va);
    p->pages_mapped -= (old_top - new_top) / kPage;
  }
  p->brk_end = new_end;
  return new_end;
}

int64_t mmap(Process* p, uint64_t length, uint64_t prot) {
  if (length == 0 || length > (1ull << 30) || (prot & ~(kProtRead | kProtWrite))) return -kEINVAL;
  uint64_t pages = os::align_up(length, kPage) / kPage;
  if (p->pages_mapped + pages > kPageLimit) return -kENOMEM;
  MmapRegion* slot = nullptr;
  for (auto& r : p->regions)
    if (!r.pages) {
      slot = &r;
      break;
    }
  if (!slot) return -kENOMEM;
  uint64_t start = p->mmap_next;
  uint64_t flags = kUserPage | vmm::kNoExecute | ((prot & kProtWrite) ? vmm::kWritable : 0);
  for (uint64_t i = 0; i < pages; i++) {
    if (!map_zeroed(p->as, start + i * kPage, flags)) {
      for (uint64_t j = 0; j < i; j++) p->as->unmap(start + j * kPage);
      return -kENOMEM;
    }
  }
  p->mmap_next += (pages + 1) * kPage;  // leave an unmapped page between regions
  *slot = {start, pages};
  p->pages_mapped += pages;
  return static_cast<int64_t>(start);
}

int64_t munmap(Process* p, uint64_t addr, uint64_t length) {
  uint64_t pages = os::align_up(length, kPage) / kPage;
  for (auto& r : p->regions) {
    if (r.pages && r.start == addr && r.pages == pages) {
      for (uint64_t i = 0; i < pages; i++) p->as->unmap(addr + i * kPage);
      p->pages_mapped -= pages;
      r = {0, 0};
      return 0;
    }
  }
  return -kEINVAL;  // only whole regions returned by mmap can be unmapped
}

}  // namespace process
