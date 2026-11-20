// Small deterministic PRNG (xoshiro256**) for tests, benchmarks and fuzzers.
#pragma once

#include <stdint.h>

namespace os {

class Random {
 public:
  explicit Random(uint64_t seed) {
    // splitmix64 to spread the seed over the state.
    for (auto& w : s_) {
      seed += 0x9E3779B97F4A7C15ull;
      uint64_t z = seed;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      w = z ^ (z >> 31);
    }
  }
  uint64_t next() {
    uint64_t result = rotl(s_[1] * 5, 7) * 9;
    uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }
  // Uniform in [0, n). n must be > 0.
  uint64_t below(uint64_t n) { return static_cast<uint64_t>((static_cast<unsigned __int128>(next()) * n) >> 64); }
  bool chance(uint32_t num, uint32_t den) { return below(den) < num; }

 private:
  static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
  uint64_t s_[4];
};

}  // namespace os
