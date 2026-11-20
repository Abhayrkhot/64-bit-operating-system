#include "common/blockdevs.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>

namespace host {

FileDevice::~FileDevice() {
  if (fd_ >= 0) ::close(fd_);
}

int FileDevice::open(const std::string& path, bool create, uint64_t bytes) {
  fd_ = ::open(path.c_str(), O_RDWR | (create ? O_CREAT | O_TRUNC : 0), 0644);
  if (fd_ < 0) return -errno;
  if (create && ftruncate(fd_, static_cast<off_t>(bytes)) < 0) return -errno;
  off_t size = lseek(fd_, 0, SEEK_END);
  if (size < 0) return -errno;
  blocks_ = static_cast<uint32_t>(static_cast<uint64_t>(size) / kBlockSize);
  return 0;
}

int FileDevice::read(uint32_t block, uint32_t count, void* buf) {
  if (block + static_cast<uint64_t>(count) > blocks_) return -EIO;
  size_t n = static_cast<size_t>(count) * kBlockSize;
  ssize_t r = pread(fd_, buf, n, static_cast<off_t>(block) * kBlockSize);
  return r == static_cast<ssize_t>(n) ? 0 : -EIO;
}

int FileDevice::write(uint32_t block, uint32_t count, const void* buf) {
  if (block + static_cast<uint64_t>(count) > blocks_) return -EIO;
  size_t n = static_cast<size_t>(count) * kBlockSize;
  ssize_t r = pwrite(fd_, buf, n, static_cast<off_t>(block) * kBlockSize);
  return r == static_cast<ssize_t>(n) ? 0 : -EIO;
}

int FileDevice::flush() { return fdatasync(fd_) == 0 ? 0 : -errno; }

int MemDevice::read(uint32_t block, uint32_t count, void* buf) {
  if (block + static_cast<uint64_t>(count) > blocks_) return -EIO;
  std::memcpy(buf, data_.data() + static_cast<size_t>(block) * kBlockSize, static_cast<size_t>(count) * kBlockSize);
  return 0;
}

int MemDevice::write(uint32_t block, uint32_t count, const void* buf) {
  if (block + static_cast<uint64_t>(count) > blocks_) return -EIO;
  std::memcpy(data_.data() + static_cast<size_t>(block) * kBlockSize, buf, static_cast<size_t>(count) * kBlockSize);
  return 0;
}

int RecordingDevice::write(uint32_t block, uint32_t count, const void* buf) {
  int r = inner_->write(block, count, buf);
  if (r < 0) return r;
  auto* p = static_cast<const uint8_t*>(buf);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t idx = static_cast<uint32_t>(payloads_.size() / kBlockSize);
    payloads_.insert(payloads_.end(), p + static_cast<size_t>(i) * kBlockSize,
                     p + static_cast<size_t>(i + 1) * kBlockSize);
    events_.push_back({false, block + i, idx});
  }
  return 0;
}

int RecordingDevice::flush() {
  int r = inner_->flush();
  if (r < 0) return r;
  events_.push_back({true, 0, 0});
  return 0;
}

uint8_t* OverlayDevice::slot(uint32_t block) {
  auto& s = blocks_[block];
  if (!s) {
    s.reset(new uint8_t[kBlockSize]);
    std::memcpy(s.get(), base_->block(block), kBlockSize);
  }
  return s.get();
}

int OverlayDevice::read(uint32_t block, uint32_t count, void* buf) {
  if (block + static_cast<uint64_t>(count) > block_count()) return -EIO;
  auto* out = static_cast<uint8_t*>(buf);
  for (uint32_t i = 0; i < count; i++) {
    auto it = blocks_.find(block + i);
    std::memcpy(out + static_cast<size_t>(i) * kBlockSize, it != blocks_.end() ? it->second.get() : base_->block(block + i),
                kBlockSize);
  }
  return 0;
}

int OverlayDevice::write(uint32_t block, uint32_t count, const void* buf) {
  if (block + static_cast<uint64_t>(count) > block_count()) return -EIO;
  auto* in = static_cast<const uint8_t*>(buf);
  for (uint32_t i = 0; i < count; i++)
    std::memcpy(slot(block + i), in + static_cast<size_t>(i) * kBlockSize, kBlockSize);
  return 0;
}

void OverlayDevice::write_sectors(uint32_t block, const uint8_t* data, uint32_t first_sector, uint32_t sectors) {
  uint8_t* s = slot(block);
  std::memcpy(s + first_sector * 512, data + first_sector * 512, sectors * 512);
}

}  // namespace host
