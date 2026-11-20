// Validation of untrusted user ELF executables before the kernel maps them.
// Portable: the host fuzzer (host/fuzz/elf_fuzz.cpp) runs this same code.
//
// Accepted: static ET_EXEC x86-64 ELF64, at most kMaxSegments PT_LOAD
// segments, each inside [user_lo, user_hi), no writable+executable segment,
// no two segments sharing a page, file ranges inside the file, entry point
// inside an executable segment. All arithmetic is overflow checked.
#pragma once

#include <stdint.h>

#include <abi/elf.hpp>

namespace os::elf {

constexpr unsigned kMaxSegments = 8;
constexpr unsigned kMaxPhdrs = 32;

struct Segment {
  uint64_t vaddr, memsz;
  uint64_t offset, filesz;
  uint32_t flags;  // kPfR/kPfW/kPfX
};

struct Image {
  uint64_t entry;
  Segment segments[kMaxSegments];
  unsigned count;
  uint64_t lowest;   // page-aligned
  uint64_t highest;  // page-aligned end of the highest segment
};

enum class Error {
  kOk,
  kTooSmall,
  kBadIdent,
  kNotExecutable,
  kWrongMachine,
  kBadProgramHeaders,
  kDynamic,
  kBadSegment,
  kOutOfRange,
  kWriteAndExecute,
  kOverlap,
  kTooManySegments,
  kNoSegments,
  kBadEntry,
};

Error validate(const uint8_t* data, uint64_t size, uint64_t user_lo, uint64_t user_hi, Image* out);
const char* error_name(Error e);

}  // namespace os::elf
