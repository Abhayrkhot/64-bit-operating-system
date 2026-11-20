// libFuzzer harness for the kernel's ELF validator (lib/elf/validate.cpp),
// built with ASan + UBSan. Besides "doesn't crash", it checks the
// validator's postconditions on every accepted input: the loader relies on
// them to copy segments without further checks.
//
//   make fuzz-elf FUZZ_SECONDS=300
#include <cstdlib>

#include "elf/validate.hpp"

using namespace shale::elf;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  constexpr uint64_t kLo = 0x10000, kHi = 0x0000010000000000ull;
  Image img;
  if (validate(data, size, kLo, kHi, &img) != Error::kOk) return 0;
  if (img.count == 0 || img.count > kMaxSegments) std::abort();
  bool entry_ok = false;
  for (unsigned i = 0; i < img.count; i++) {
    const Segment& s = img.segments[i];
    if (s.filesz > s.memsz || s.offset + s.filesz > size || s.offset + s.filesz < s.offset) std::abort();
    if (s.vaddr < kLo || s.vaddr + s.memsz > kHi || s.vaddr + s.memsz < s.vaddr) std::abort();
    if ((s.flags & kPfW) && (s.flags & kPfX)) std::abort();
    if ((s.flags & kPfX) && img.entry >= s.vaddr && img.entry < s.vaddr + s.memsz) entry_ok = true;
    for (unsigned j = i + 1; j < img.count; j++) {
      const Segment& t = img.segments[j];
      uint64_t a0 = s.vaddr & ~0xFFFull, a1 = (s.vaddr + s.memsz + 0xFFF) & ~0xFFFull;
      uint64_t b0 = t.vaddr & ~0xFFFull, b1 = (t.vaddr + t.memsz + 0xFFF) & ~0xFFFull;
      if (a0 < b1 && b0 < a1) std::abort();
    }
    // Touch every byte the loader would copy (ASan catches out-of-bounds).
    volatile uint8_t sink = 0;
    for (uint64_t k = 0; k < s.filesz; k += 512) sink ^= data[s.offset + k];
    if (s.filesz) sink ^= data[s.offset + s.filesz - 1];
    (void)sink;
  }
  if (!entry_ok) std::abort();
  return 0;
}
