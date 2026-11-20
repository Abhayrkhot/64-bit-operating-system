#include "util/crc32c.hpp"

namespace os {
namespace {

struct Table {
  uint32_t v[256];
  constexpr Table() : v() {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
      v[i] = c;
    }
  }
};
constexpr Table kTable;

}  // namespace

uint32_t crc32c(uint32_t crc, const void* data, size_t n) {
  auto* p = static_cast<const uint8_t*>(data);
  crc = ~crc;
  for (size_t i = 0; i < n; i++) crc = kTable.v[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

}  // namespace os
