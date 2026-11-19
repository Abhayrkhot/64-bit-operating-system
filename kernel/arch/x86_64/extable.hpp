// Exception table: instructions that are allowed to fault in kernel mode.
// Each entry maps a faulting RIP to a fixup RIP; the fixup code reports the
// failure (EFAULT) to the caller. Used by user-memory copies and by tests
// that probe page permissions.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace interrupts {
struct Frame;
}

namespace extable {

struct Entry {
  uint64_t insn;
  uint64_t fixup;
};

// Returns true and redirects the frame if f->rip has an entry.
bool fixup(interrupts::Frame* f);

}  // namespace extable

// Probes (extable.S): return 0 if the access succeeded, 1 if it faulted.
extern "C" int probe_read(const void* addr);
extern "C" int probe_write(void* addr);  // rewrites the byte it read
// Calls addr (expected to fault on fetch, or to be a function returning 0).
extern "C" int probe_exec(const void* addr);
