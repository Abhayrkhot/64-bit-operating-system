#include "mm/kheap.hpp"

#include "common/mem.hpp"
#include "core/boot.hpp"
#include "core/log.hpp"
#include "mm/pmm.hpp"

namespace {
os::mm::Heap g_heap;
}

void kheap_init() { g_heap.init(&pmm::buddy(), phys_to_virt<uint8_t>(0)); }

void* kmalloc(size_t size) {
  LockGuard guard(pmm::lock());
  return g_heap.alloc(size);
}

void* kzalloc(size_t size) {
  void* p = kmalloc(size);
  if (p) memset(p, 0, size);
  return p;
}

int kcache_create(uint32_t object_size) {
  LockGuard guard(pmm::lock());
  return g_heap.create_cache(object_size);
}

void* kcache_alloc(int cache) {
  LockGuard guard(pmm::lock());
  return g_heap.alloc_from(cache);
}

bool kfree_checked(void* p) {
  LockGuard guard(pmm::lock());
  return g_heap.free(p);
}

void kfree(void* p) {
  if (!kfree_checked(p)) panic("kfree: invalid pointer %p", p);
}

size_t kmalloc_usable_size(const void* p) {
  LockGuard guard(pmm::lock());
  return g_heap.usable_size(p);
}

os::mm::Heap::Stats kheap_stats() {
  LockGuard guard(pmm::lock());
  return g_heap.stats();
}

os::mm::Heap::ClassStats kheap_class_stats(unsigned index) {
  LockGuard guard(pmm::lock());
  return g_heap.class_stats(index);
}

bool kheap_check() {
  LockGuard guard(pmm::lock());
  return g_heap.check_invariants() && pmm::buddy().check_invariants();
}

// C++ allocation. The kernel treats running out of memory in `new` as fatal;
// code that can recover calls kmalloc and placement new.
void* operator new(size_t size) {
  void* p = kmalloc(size);
  if (!p) panic("operator new(%zu): out of memory", size);
  return p;
}
void* operator new[](size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { kfree(p); }
void operator delete[](void* p) noexcept { kfree(p); }
void operator delete(void* p, size_t) noexcept { kfree(p); }
void operator delete[](void* p, size_t) noexcept { kfree(p); }
