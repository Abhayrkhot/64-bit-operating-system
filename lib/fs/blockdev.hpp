// Block device interface (4 KiB blocks) used by ShaleFS. The kernel backs it
// with virtio-blk; host tools with files, memory, or a recording device that
// logs writes and flush barriers for crash simulation.
//
// Contract the file system relies on: a write may be reordered with any
// other write until a flush() returns; after flush() returns, every write
// issued before it is durable.
#pragma once

#include <stdint.h>

namespace shale::fs {

class BlockDevice {
 public:
  virtual ~BlockDevice() = default;
  virtual uint32_t block_count() const = 0;
  // Return 0 or -errno.
  virtual int read(uint32_t block, uint32_t count, void* buf) = 0;
  virtual int write(uint32_t block, uint32_t count, const void* buf) = 0;
  virtual int flush() = 0;
  // Reads several (not necessarily contiguous) blocks; devices that can keep
  // many requests in flight override this. bufs[i] receives blocks[i].
  virtual int read_batch(const uint32_t* blocks, uint32_t n, uint8_t* const* bufs) {
    for (uint32_t i = 0; i < n; i++) {
      int r = read(blocks[i], 1, bufs[i]);
      if (r < 0) return r;
    }
    return 0;
  }
};

}  // namespace shale::fs
