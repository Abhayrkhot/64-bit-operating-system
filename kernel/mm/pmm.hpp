// Physical memory manager: the buddy allocator over all RAM frames.
#pragma once

#include <stdint.h>

#include "mm/buddy.hpp"
#include "sync/spinlock.hpp"

namespace pmm {

void init();
// Frees loader and boot-data memory. Call once the kernel runs on its own
// GDT, IDT and page tables.
void reclaim_boot_memory();

// Physical address of a naturally aligned 2^order-page block, or 0 when out
// of memory (frame 0 is never handed out).
uint64_t alloc(unsigned order = 0);
uint64_t alloc_zeroed(unsigned order = 0);
void free(uint64_t phys, unsigned order = 0);

uint64_t free_bytes();
uint64_t managed_bytes();
uint64_t metadata_bytes();

// The heap shares the allocator and its lock.
os::mm::BuddyAllocator& buddy();
SpinLock& lock();

}  // namespace pmm
