#include "elf/validate.hpp"

#include "common/mem.hpp"

namespace shale::elf {
namespace {

constexpr uint64_t kPage = 4096;
constexpr uint32_t kPtInterp = 3, kPtDynamic = 2;

bool add_overflows(uint64_t a, uint64_t b, uint64_t* out) { return __builtin_add_overflow(a, b, out); }

}  // namespace

const char* error_name(Error e) {
  switch (e) {
    case Error::kOk: return "ok";
    case Error::kTooSmall: return "file too small";
    case Error::kBadIdent: return "not an ELF64 little-endian file";
    case Error::kNotExecutable: return "not ET_EXEC";
    case Error::kWrongMachine: return "not x86-64";
    case Error::kBadProgramHeaders: return "bad program header table";
    case Error::kDynamic: return "dynamic executables are not supported";
    case Error::kBadSegment: return "segment file range invalid";
    case Error::kOutOfRange: return "segment outside user space";
    case Error::kWriteAndExecute: return "writable and executable segment";
    case Error::kOverlap: return "segments overlap";
    case Error::kTooManySegments: return "too many segments";
    case Error::kNoSegments: return "no loadable segments";
    case Error::kBadEntry: return "entry point not in an executable segment";
  }
  return "?";
}

Error validate(const uint8_t* data, uint64_t size, uint64_t user_lo, uint64_t user_hi, Image* out) {
  if (size < sizeof(Ehdr)) return Error::kTooSmall;
  Ehdr eh;
  memcpy(&eh, data, sizeof(eh));  // the file may be unaligned
  if (!has_valid_ident(eh) || eh.version != 1) return Error::kBadIdent;
  if (eh.type != kTypeExec) return Error::kNotExecutable;
  if (eh.machine != kMachineX8664) return Error::kWrongMachine;
  if (eh.phentsize != sizeof(Phdr) || eh.phnum == 0 || eh.phnum > kMaxPhdrs) return Error::kBadProgramHeaders;
  uint64_t table_end;
  if (add_overflows(eh.phoff, static_cast<uint64_t>(eh.phnum) * sizeof(Phdr), &table_end) || table_end > size)
    return Error::kBadProgramHeaders;

  Image img{};
  img.entry = eh.entry;
  img.lowest = ~0ull;
  for (unsigned i = 0; i < eh.phnum; i++) {
    Phdr ph;
    memcpy(&ph, data + eh.phoff + i * sizeof(Phdr), sizeof(ph));
    if (ph.type == kPtInterp || ph.type == kPtDynamic) return Error::kDynamic;
    if (ph.type != kPtLoad || ph.memsz == 0) continue;
    if (img.count == kMaxSegments) return Error::kTooManySegments;
    uint64_t file_end, mem_end;
    if (ph.filesz > ph.memsz || add_overflows(ph.offset, ph.filesz, &file_end) || file_end > size)
      return Error::kBadSegment;
    if (ph.vaddr < user_lo || add_overflows(ph.vaddr, ph.memsz, &mem_end) || mem_end > user_hi)
      return Error::kOutOfRange;
    if ((ph.flags & kPfW) && (ph.flags & kPfX)) return Error::kWriteAndExecute;
    img.segments[img.count++] = {ph.vaddr, ph.memsz, ph.offset, ph.filesz, ph.flags};
  }
  if (img.count == 0) return Error::kNoSegments;

  // No two segments may touch the same page: each page gets one set of permissions.
  for (unsigned i = 0; i < img.count; i++) {
    const Segment& a = img.segments[i];
    uint64_t a_lo = a.vaddr & ~(kPage - 1), a_hi = align_up(a.vaddr + a.memsz, kPage);
    if (a_hi < a.vaddr + a.memsz) return Error::kOutOfRange;  // align_up wrapped
    for (unsigned j = i + 1; j < img.count; j++) {
      const Segment& b = img.segments[j];
      uint64_t b_lo = b.vaddr & ~(kPage - 1), b_hi = align_up(b.vaddr + b.memsz, kPage);
      if (a_lo < b_hi && b_lo < a_hi) return Error::kOverlap;
    }
    if (a_lo < img.lowest) img.lowest = a_lo;
    if (a_hi > img.highest) img.highest = a_hi;
  }

  bool entry_ok = false;
  for (unsigned i = 0; i < img.count; i++) {
    const Segment& s = img.segments[i];
    if ((s.flags & kPfX) && eh.entry >= s.vaddr && eh.entry < s.vaddr + s.memsz) entry_ok = true;
  }
  if (!entry_ok) return Error::kBadEntry;
  *out = img;
  return Error::kOk;
}

}  // namespace shale::elf
