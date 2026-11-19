// Kernel heap (slab + page runs, see lib/mm/heap.hpp) over the PMM.
#pragma once

#include <stddef.h>

#include "mm/heap.hpp"

void kheap_init();
void* kmalloc(size_t size);
void* kzalloc(size_t size);
void kfree(void* p);
// Dedicated slab cache for one fixed-size kernel object type. Objects are
// freed with kfree.
int kcache_create(uint32_t object_size);
void* kcache_alloc(int cache);
// kfree that reports instead of panicking on pointers the heap didn't hand out.
bool kfree_checked(void* p);
size_t kmalloc_usable_size(const void* p);
shale::mm::Heap::Stats kheap_stats();
shale::mm::Heap::ClassStats kheap_class_stats(unsigned index);
bool kheap_check();
