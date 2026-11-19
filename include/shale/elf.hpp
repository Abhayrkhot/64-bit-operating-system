// ELF64 definitions shared by the UEFI loader, the kernel's program loader and
// host tools. Only what Shale uses.
#pragma once

#include <stdint.h>

namespace shale::elf {

constexpr uint8_t kMag0 = 0x7f;
constexpr uint8_t kClass64 = 2;
constexpr uint8_t kData2Lsb = 1;
constexpr uint16_t kTypeExec = 2;
constexpr uint16_t kMachineX8664 = 62;

constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kPfX = 1;
constexpr uint32_t kPfW = 2;
constexpr uint32_t kPfR = 4;

struct Ehdr {
  uint8_t ident[16];
  uint16_t type;
  uint16_t machine;
  uint32_t version;
  uint64_t entry;
  uint64_t phoff;
  uint64_t shoff;
  uint32_t flags;
  uint16_t ehsize;
  uint16_t phentsize;
  uint16_t phnum;
  uint16_t shentsize;
  uint16_t shnum;
  uint16_t shstrndx;
};

struct Phdr {
  uint32_t type;
  uint32_t flags;
  uint64_t offset;
  uint64_t vaddr;
  uint64_t paddr;
  uint64_t filesz;
  uint64_t memsz;
  uint64_t align;
};

static_assert(sizeof(Ehdr) == 64, "Ehdr layout");
static_assert(sizeof(Phdr) == 56, "Phdr layout");

inline bool has_valid_ident(const Ehdr& h) {
  return h.ident[0] == kMag0 && h.ident[1] == 'E' && h.ident[2] == 'L' && h.ident[3] == 'F' &&
         h.ident[4] == kClass64 && h.ident[5] == kData2Lsb;
}

}  // namespace shale::elf
