// Read-only initial ramdisk: a ustar archive loaded by the UEFI loader.
// Programs live under /bin.
#pragma once

#include <stdint.h>

namespace initrd {

void init(uint64_t phys, uint64_t size);
// path like "/bin/hello". Returns false if absent.
bool find(const char* path, const uint8_t** data, uint64_t* size);
// Iterates regular files: index -> name ("bin/hello"), false past the end.
bool entry(uint64_t index, const char** name, uint64_t* size);

}  // namespace initrd
