// Minimal growable array for code shared with the kernel (no STL there).
// Elements must be trivially copyable.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "common/mem.hpp"

namespace shale {

template <typename T>
class Vec {
 public:
  Vec() = default;
  Vec(const Vec&) = delete;
  Vec& operator=(const Vec&) = delete;
  ~Vec() { delete[] reinterpret_cast<uint8_t*>(data_); }

  bool push(const T& v) {
    if (size_ == cap_ && !grow()) return false;
    data_[size_++] = v;
    return true;
  }
  void clear() { size_ = 0; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  T& operator[](size_t i) { return data_[i]; }
  const T& operator[](size_t i) const { return data_[i]; }
  T* begin() { return data_; }
  T* end() { return data_ + size_; }
  void pop() { size_--; }
  T& back() { return data_[size_ - 1]; }

 private:
  bool grow() {
    size_t cap = cap_ ? cap_ * 2 : 16;
    auto* bytes = new uint8_t[cap * sizeof(T)];
    if (!bytes) return false;
    if (size_) memcpy(bytes, data_, size_ * sizeof(T));
    delete[] reinterpret_cast<uint8_t*>(data_);
    data_ = reinterpret_cast<T*>(bytes);
    cap_ = cap;
    return true;
  }
  T* data_ = nullptr;
  size_t size_ = 0, cap_ = 0;
};

}  // namespace shale
