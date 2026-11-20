#include "fs/file.hpp"

#include "arch/x86_64/cpu.hpp"
#include "common/mem.hpp"
#include "mm/kheap.hpp"

namespace {
int g_file_cache = -1;
}

File* file_alloc(const FileOps* ops, uint32_t type, uint64_t flags, void* priv) {
  if (g_file_cache < 0) g_file_cache = kcache_create(sizeof(File));
  auto* f = static_cast<File*>(kcache_alloc(g_file_cache));
  if (!f) return nullptr;
  memset(f, 0, sizeof(*f));
  f->ops = ops;
  f->refs = 1;
  f->type = type;
  f->flags = flags;
  f->priv = priv;
  return f;
}

File* file_get(File* f) {
  bool irq = arch::irq_save();
  f->refs++;
  arch::irq_restore(irq);
  return f;
}

void file_put(File* f) {
  bool irq = arch::irq_save();
  bool last = --f->refs == 0;
  arch::irq_restore(irq);
  if (!last) return;
  if (f->ops->release) f->ops->release(f);
  kfree(f);
}
