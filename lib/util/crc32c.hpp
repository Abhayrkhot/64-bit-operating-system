// CRC32C (Castagnoli), table driven. Used for journal commit checksums and
// superblock integrity.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace shale {

uint32_t crc32c(uint32_t crc, const void* data, size_t n);
inline uint32_t crc32c(const void* data, size_t n) { return crc32c(0, data, n); }

}  // namespace shale
