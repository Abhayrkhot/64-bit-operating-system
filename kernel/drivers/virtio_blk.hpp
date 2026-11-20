// virtio-blk over legacy virtio-pci (QEMU: -device virtio-blk-pci,disable-modern=on).
//
// Implements shale::fs::BlockDevice. Writes are asynchronous: each one is
// copied into a bounce buffer and queued, so the caller may reuse its buffer
// at once and the device may complete writes in any order. flush() waits for
// every outstanding request and then issues VIRTIO_BLK_T_FLUSH, which is the
// ordering point the file system builds on. Reads are synchronous.
//
// Completion is polled (busy, then yielding to other threads, then sleeping
// for very slow requests) rather than interrupt-driven, which keeps the
// driver independent of INTx routing. Requests are queued behind a batched
// doorbell: each notify is a VM exit.
#pragma once

#include "fs/blockdev.hpp"

namespace virtio_blk {

// Probes PCI; returns the first virtio-blk disk or nullptr.
shale::fs::BlockDevice* probe();

struct Stats {
  uint64_t reads, writes, flushes, polls_slept;
};
Stats stats();

}  // namespace virtio_blk
