// Address-space isolation: several children write their own pattern into the
// same virtual addresses (.data, heap, mmap) and keep checking it while the
// scheduler interleaves them. Any cross-process leak shows up as a mismatch.
#include "libc.hpp"

namespace {
uint8_t g_buf[64 * 1024];

int child(char tag) {
  auto* heap = static_cast<uint8_t*>(malloc(32 * 1024));
  int64_t map = mmap(16 * 4096, sys::kProtRead | sys::kProtWrite);
  if (!heap || map < 0) return 10;
  auto* mapped = reinterpret_cast<uint8_t*>(map);
  for (int round = 0; round < 200; round++) {
    uint8_t v = static_cast<uint8_t>(tag + round);
    memset(g_buf, v, sizeof(g_buf));
    memset(heap, v, 32 * 1024);
    memset(mapped, v, 16 * 4096);
    yield();
    for (size_t i = 0; i < sizeof(g_buf); i += 97)
      if (g_buf[i] != v) return 11;
    for (size_t i = 0; i < 32 * 1024; i += 97)
      if (heap[i] != v) return 12;
    for (size_t i = 0; i < 16 * 4096; i += 97)
      if (mapped[i] != v) return 13;
  }
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && !strcmp(argv[1], "child")) return child(argv[2][0]);
  const char* tags[] = {"A", "B", "C", "D"};
  int pids[4];
  for (int i = 0; i < 4; i++) {
    const char* args[] = {"isolation", "child", tags[i], nullptr};
    pids[i] = spawn("/bin/isolation", args);
    if (pids[i] < 0) {
      printf("isolation: spawn failed: %s\n", errno_name(pids[i]));
      return 1;
    }
  }
  int failures = 0;
  for (int i = 0; i < 4; i++) {
    int status = -1;
    if (wait(pids[i], &status) != pids[i] || status != 0) {
      printf("isolation: child %s exited with %d\n", tags[i], status);
      failures++;
    }
  }
  printf("isolation: %d processes shared virtual addresses without interference%s\n", 4,
         failures ? " -- FAILED" : "");
  return failures ? 1 : 0;
}
