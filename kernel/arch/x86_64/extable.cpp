#include "arch/x86_64/extable.hpp"

#include "arch/x86_64/interrupts.hpp"

extern "C" const extable::Entry __extable_start[], __extable_end[];
extern "C" uint64_t probe_exec_target;
extern "C" char probe_exec_fault[];

namespace extable {

bool fixup(interrupts::Frame* f) {
  for (const Entry* e = __extable_start; e < __extable_end; e++) {
    if (e->insn == f->rip) {
      f->rip = e->fixup;
      return true;
    }
  }
  if (probe_exec_target && f->rip == probe_exec_target && f->vector == interrupts::kVectorPageFault) {
    f->rip = reinterpret_cast<uint64_t>(probe_exec_fault);
    return true;
  }
  return false;
}

}  // namespace extable
