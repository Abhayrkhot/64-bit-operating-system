// Checked access to user memory from system calls. These are the only ways
// kernel code touches user pointers.
//
//  1. Range check: [addr, addr + n) must lie in the user half, with overflow
//     checked, so a user can't point the kernel at kernel memory.
//  2. The copy itself runs with SMAP opened only for its duration, and any
//     page fault inside it resumes at an exception-table fixup returning
//     -EFAULT (unmapped pages, writes to read-only pages).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace usercopy {

void init();
bool range_ok(uint64_t addr, uint64_t n);
int64_t from_user(void* dst, uint64_t src, uint64_t n);
int64_t to_user(uint64_t dst, const void* src, uint64_t n);
// Copies a NUL-terminated string of at most max-1 characters.
int64_t strncpy_from_user(char* dst, uint64_t src, uint64_t max);

}  // namespace usercopy
