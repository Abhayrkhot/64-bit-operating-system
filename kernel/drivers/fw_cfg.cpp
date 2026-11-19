#include "drivers/fw_cfg.hpp"

#include "arch/x86_64/cpu.hpp"
#include "common/mem.hpp"

namespace fw_cfg {
namespace {
constexpr uint16_t kSelector = 0x510;
constexpr uint16_t kData = 0x511;
constexpr uint16_t kSignature = 0x0000;
constexpr uint16_t kFileDir = 0x0019;

void select(uint16_t key) { arch::outw(kSelector, key); }
uint8_t byte() { return arch::inb(kData); }
uint32_t be32() {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v = (v << 8) | byte();
  return v;
}
uint16_t be16() { return static_cast<uint16_t>((byte() << 8) | byte()); }
}  // namespace

bool present() {
  select(kSignature);
  char sig[4];
  for (char& c : sig) c = static_cast<char>(byte());
  return sig[0] == 'Q' && sig[1] == 'E' && sig[2] == 'M' && sig[3] == 'U';
}

long read_file(const char* name, char* buf, size_t size) {
  if (!present()) return -1;
  select(kFileDir);
  uint32_t count = be32();
  for (uint32_t i = 0; i < count; i++) {
    uint32_t file_size = be32();
    uint16_t key = be16();
    be16();  // reserved
    char entry_name[56];
    for (char& c : entry_name) c = static_cast<char>(byte());
    entry_name[55] = 0;
    if (strcmp(entry_name, name) != 0) continue;
    select(key);
    size_t n = file_size < size - 1 ? file_size : size - 1;
    for (size_t j = 0; j < n; j++) buf[j] = static_cast<char>(byte());
    buf[n] = 0;
    return static_cast<long>(file_size);
  }
  return -1;
}

}  // namespace fw_cfg
