#include "drivers/virtio_blk.hpp"

#include <shale/syscall.hpp>

#include "arch/x86_64/cpu.hpp"
#include "common/mem.hpp"
#include "core/boot.hpp"
#include "core/log.hpp"
#include "drivers/pci.hpp"
#include "fs/format.hpp"
#include "mm/kheap.hpp"
#include "mm/pmm.hpp"
#include "sched/sched.hpp"

namespace virtio_blk {
namespace {

using shale::fs::kBlockSize;

constexpr uint16_t kVendorVirtio = 0x1AF4, kDeviceLegacyBlock = 0x1001;
// Legacy virtio-pci register block (BAR0, I/O space), without MSI-X.
constexpr uint16_t kRegHostFeatures = 0x00, kRegGuestFeatures = 0x04, kRegQueuePfn = 0x08, kRegQueueSize = 0x0C,
                   kRegQueueSelect = 0x0E, kRegQueueNotify = 0x10, kRegStatus = 0x12, kRegIsr = 0x13,
                   kRegConfig = 0x14;
constexpr uint8_t kStatusAck = 1, kStatusDriver = 2, kStatusDriverOk = 4;
constexpr uint32_t kFeatureFlush = 1u << 9;
constexpr uint32_t kReqIn = 0, kReqOut = 1, kReqFlush = 4;
constexpr uint16_t kDescNext = 1, kDescWrite = 2;
constexpr uint16_t kAvailNoInterrupt = 1;

struct Desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};
struct UsedElem {
  uint32_t id;
  uint32_t len;
};
struct ReqHeader {
  uint32_t type;
  uint32_t reserved;
  uint64_t sector;
};

constexpr unsigned kMaxSlots = 64;
struct Slot {
  alignas(16) ReqHeader hdr;
  volatile uint8_t status;
  bool busy;
  volatile bool done;
  uint16_t head;
  uint8_t* bounce;  // one 4 KiB block, page aligned
};

Stats g_stats;

void barrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

class Disk final : public shale::fs::BlockDevice {
 public:
  bool init(const pci::Device& dev);
  uint32_t block_count() const override { return blocks_; }
  int read(uint32_t block, uint32_t count, void* buf) override;
  int write(uint32_t block, uint32_t count, const void* buf) override;
  int flush() override;

 private:
  int submit(Slot* s, uint32_t type, uint64_t sector, bool with_data, bool device_writes);
  void reap();
  int wait(Slot* s);
  Slot* acquire();
  void release(Slot* s) { s->busy = false; }

  uint16_t io_ = 0;
  uint16_t qsize_ = 0;
  Desc* desc_ = nullptr;
  volatile uint16_t* avail_ = nullptr;  // flags, idx, ring[qsize]
  volatile uint16_t* used_hdr_ = nullptr;  // flags, idx
  volatile UsedElem* used_ = nullptr;
  uint16_t free_head_ = 0, num_free_ = 0, last_used_ = 0, avail_idx_ = 0;
  uint16_t slot_of_head_[1024];
  Slot slots_[kMaxSlots];
  unsigned nslots_ = 0;
  bool can_flush_ = false;
  bool write_error_ = false;
  uint32_t blocks_ = 0;
  Mutex lock_;
};

bool Disk::init(const pci::Device& dev) {
  if (!(dev.bar[0] & 1)) return false;  // legacy device: BAR0 must be I/O space
  io_ = static_cast<uint16_t>(dev.bar[0] & ~3u);
  pci::enable(dev);
  arch::outb(io_ + kRegStatus, 0);  // reset
  arch::outb(io_ + kRegStatus, kStatusAck);
  arch::outb(io_ + kRegStatus, kStatusAck | kStatusDriver);
  uint32_t features = arch::inl(io_ + kRegHostFeatures);
  can_flush_ = features & kFeatureFlush;
  arch::outl(io_ + kRegGuestFeatures, features & kFeatureFlush);
  arch::outw(io_ + kRegQueueSelect, 0);
  qsize_ = arch::inw(io_ + kRegQueueSize);
  if (qsize_ == 0 || qsize_ > 1024) return false;
  // Legacy ring layout: descriptors, then the available ring, then (page
  // aligned) the used ring. One physically contiguous, zeroed allocation.
  uint64_t avail_off = 16ull * qsize_;
  uint64_t used_off = shale::align_up(avail_off + 6 + 2ull * qsize_, 4096);
  uint64_t bytes = used_off + shale::align_up(6 + 8ull * qsize_, 4096);
  unsigned order = 0;
  while ((4096ull << order) < bytes) order++;
  uint64_t ring = pmm::alloc_zeroed(order);
  if (!ring) return false;
  auto* base = phys_to_virt<uint8_t>(ring);
  desc_ = reinterpret_cast<Desc*>(base);
  avail_ = reinterpret_cast<volatile uint16_t*>(base + avail_off);
  used_hdr_ = reinterpret_cast<volatile uint16_t*>(base + used_off);
  used_ = reinterpret_cast<volatile UsedElem*>(base + used_off + 4);
  for (uint16_t i = 0; i < qsize_; i++) desc_[i].next = static_cast<uint16_t>(i + 1);
  free_head_ = 0;
  num_free_ = qsize_;
  avail_[0] = kAvailNoInterrupt;  // we poll the used ring
  arch::outl(io_ + kRegQueuePfn, static_cast<uint32_t>(ring >> 12));
  nslots_ = shale::min<unsigned>(kMaxSlots, qsize_ / 3);
  for (unsigned i = 0; i < nslots_; i++) {
    slots_[i].bounce = static_cast<uint8_t*>(kmalloc(kBlockSize));
    if (!slots_[i].bounce) return false;
  }
  uint64_t sectors = arch::inl(io_ + kRegConfig) | (static_cast<uint64_t>(arch::inl(io_ + kRegConfig + 4)) << 32);
  blocks_ = static_cast<uint32_t>(shale::min<uint64_t>(sectors / 8, 0xFFFFFFF0ull));
  arch::outb(io_ + kRegStatus, kStatusAck | kStatusDriver | kStatusDriverOk);
  kprintf("virtio-blk: %lu MiB at io %x, queue %u, %u slots, flush %s\n", sectors / 2048, io_, qsize_, nslots_,
          can_flush_ ? "yes" : "no");
  return true;
}

void Disk::reap() {
  barrier();
  while (last_used_ != used_hdr_[1]) {
    uint16_t head = static_cast<uint16_t>(used_[last_used_ % qsize_].id);
    Slot* s = &slots_[slot_of_head_[head]];
    // Return the chain to the free list.
    uint16_t d = head, n = 1;
    while (desc_[d].flags & kDescNext) {
      d = desc_[d].next;
      n++;
    }
    desc_[d].next = free_head_;
    free_head_ = head;
    num_free_ = static_cast<uint16_t>(num_free_ + n);
    if (s->status != 0) write_error_ = true;
    s->done = true;
    last_used_++;
  }
  arch::inb(io_ + kRegIsr);  // acknowledge, in case the device raised one anyway
}

int Disk::submit(Slot* s, uint32_t type, uint64_t sector, bool with_data, bool device_writes) {
  uint16_t need = with_data ? 3 : 2;
  while (num_free_ < need) reap();
  s->hdr = {type, 0, sector};
  s->status = 0xFF;
  s->done = false;
  uint16_t ids[3];
  for (uint16_t i = 0; i < need; i++) {
    ids[i] = free_head_;
    free_head_ = desc_[free_head_].next;
  }
  num_free_ = static_cast<uint16_t>(num_free_ - need);
  desc_[ids[0]] = {virt_to_phys(&s->hdr), sizeof(ReqHeader), kDescNext, ids[1]};
  if (with_data) {
    desc_[ids[1]] = {virt_to_phys(s->bounce), kBlockSize,
                     static_cast<uint16_t>(kDescNext | (device_writes ? kDescWrite : 0)), ids[2]};
    desc_[ids[2]] = {virt_to_phys(const_cast<uint8_t*>(&s->status)), 1, kDescWrite, 0};
  } else {
    desc_[ids[1]] = {virt_to_phys(const_cast<uint8_t*>(&s->status)), 1, kDescWrite, 0};
  }
  s->head = ids[0];
  slot_of_head_[ids[0]] = static_cast<uint16_t>(s - slots_);
  avail_[2 + avail_idx_ % qsize_] = ids[0];
  barrier();
  avail_idx_++;
  avail_[1] = avail_idx_;
  barrier();
  arch::outw(io_ + kRegQueueNotify, 0);
  return 0;
}

int Disk::wait(Slot* s) {
  for (int spin = 0; !s->done; spin++) {
    reap();
    if (s->done) break;
    if (spin < 4000 || !sched::current()) {
      arch::pause();
    } else {
      g_stats.polls_slept++;
      sched::sleep_ms(1);
    }
  }
  return s->status == 0 ? 0 : -shale::sys::kEIO;
}

Slot* Disk::acquire() {
  for (;;) {
    for (unsigned i = 0; i < nslots_; i++) {
      Slot* s = &slots_[i];
      if (s->busy && s->done) s->busy = false;  // completed write
      if (!s->busy) {
        s->busy = true;
        return s;
      }
    }
    reap();
    arch::pause();
  }
}

int Disk::read(uint32_t block, uint32_t count, void* buf) {
  if (block + static_cast<uint64_t>(count) > blocks_) return -shale::sys::kEIO;
  lock_.lock();
  int r = 0;
  for (uint32_t i = 0; i < count && r == 0; i++) {
    Slot* s = acquire();
    submit(s, kReqIn, static_cast<uint64_t>(block + i) * 8, true, true);
    r = wait(s);
    if (r == 0) memcpy(static_cast<uint8_t*>(buf) + static_cast<uint64_t>(i) * kBlockSize, s->bounce, kBlockSize);
    release(s);
    g_stats.reads++;
  }
  lock_.unlock();
  return r;
}

int Disk::write(uint32_t block, uint32_t count, const void* buf) {
  if (block + static_cast<uint64_t>(count) > blocks_) return -shale::sys::kEIO;
  lock_.lock();
  for (uint32_t i = 0; i < count; i++) {
    Slot* s = acquire();
    memcpy(s->bounce, static_cast<const uint8_t*>(buf) + static_cast<uint64_t>(i) * kBlockSize, kBlockSize);
    submit(s, kReqOut, static_cast<uint64_t>(block + i) * 8, true, false);  // completes asynchronously
    g_stats.writes++;
  }
  lock_.unlock();
  return 0;
}

int Disk::flush() {
  lock_.lock();
  for (unsigned i = 0; i < nslots_; i++) {
    Slot* s = &slots_[i];
    if (s->busy) {
      wait(s);
      s->busy = false;
    }
  }
  int r = 0;
  if (can_flush_) {
    Slot* s = acquire();
    submit(s, kReqFlush, 0, false, false);
    r = wait(s);
    release(s);
  }
  if (write_error_) {
    write_error_ = false;
    r = -shale::sys::kEIO;
  }
  g_stats.flushes++;
  lock_.unlock();
  return r;
}

}  // namespace

shale::fs::BlockDevice* probe() {
  pci::Device dev;
  if (!pci::find(kVendorVirtio, kDeviceLegacyBlock, 0, &dev)) return nullptr;
  auto* disk = new Disk();
  if (!disk->init(dev)) {
    kprintf("virtio-blk: init failed\n");
    delete disk;
    return nullptr;
  }
  return disk;
}

Stats stats() { return g_stats; }

}  // namespace virtio_blk
