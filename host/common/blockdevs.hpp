// Host block devices for file-system tools and tests.
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "fs/blockdev.hpp"
#include "fs/format.hpp"

namespace host {

using os::fs::kBlockSize;

// A raw image file (pread/pwrite, fdatasync on flush).
class FileDevice : public os::fs::BlockDevice {
 public:
  ~FileDevice() override;
  int open(const std::string& path, bool create, uint64_t bytes = 0);
  uint32_t block_count() const override { return blocks_; }
  int read(uint32_t block, uint32_t count, void* buf) override;
  int write(uint32_t block, uint32_t count, const void* buf) override;
  int flush() override;

 private:
  int fd_ = -1;
  uint32_t blocks_ = 0;
};

class MemDevice : public os::fs::BlockDevice {
 public:
  explicit MemDevice(uint32_t blocks) : data_(static_cast<size_t>(blocks) * kBlockSize, 0), blocks_(blocks) {}
  uint32_t block_count() const override { return blocks_; }
  int read(uint32_t block, uint32_t count, void* buf) override;
  int write(uint32_t block, uint32_t count, const void* buf) override;
  int flush() override { return 0; }
  const uint8_t* block(uint32_t b) const { return data_.data() + static_cast<size_t>(b) * kBlockSize; }
  std::vector<uint8_t>& bytes() { return data_; }

 private:
  std::vector<uint8_t> data_;
  uint32_t blocks_;
};

// Forwards to an inner device and logs every block write and flush in issue
// order, which is what the crash simulator replays.
class RecordingDevice : public os::fs::BlockDevice {
 public:
  struct Event {
    bool flush;
    uint32_t block;
    uint32_t data;  // index into payloads()
  };
  explicit RecordingDevice(os::fs::BlockDevice* inner) : inner_(inner) {}
  uint32_t block_count() const override { return inner_->block_count(); }
  int read(uint32_t block, uint32_t count, void* buf) override { return inner_->read(block, count, buf); }
  int write(uint32_t block, uint32_t count, const void* buf) override;
  int flush() override;
  const std::vector<Event>& events() const { return events_; }
  const uint8_t* payload(uint32_t i) const { return payloads_.data() + static_cast<size_t>(i) * kBlockSize; }
  size_t position() const { return events_.size(); }

 private:
  os::fs::BlockDevice* inner_;
  std::vector<Event> events_;
  std::vector<uint8_t> payloads_;
};

// Copy-on-write view over a base image: crash states are a few hundred
// blocks on top of the image the workload started from.
class OverlayDevice : public os::fs::BlockDevice {
 public:
  explicit OverlayDevice(const MemDevice* base) : base_(base) {}
  uint32_t block_count() const override { return base_->block_count(); }
  int read(uint32_t block, uint32_t count, void* buf) override;
  int write(uint32_t block, uint32_t count, const void* buf) override;
  int flush() override { return 0; }
  // Applies a write to one 512-byte sector range only (torn-write simulation).
  void write_sectors(uint32_t block, const uint8_t* data, uint32_t first_sector, uint32_t sectors);

 private:
  uint8_t* slot(uint32_t block);
  const MemDevice* base_;
  std::unordered_map<uint32_t, std::unique_ptr<uint8_t[]>> blocks_;
};

}  // namespace host
