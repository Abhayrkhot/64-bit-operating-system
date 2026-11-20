// Deliberately misbehaves in one way (argv[1]); the kernel must kill only this
// process. Exits 0 only if the misbehaviour unexpectedly succeeded... which is
// a failure the test reports.
#include "libc.hpp"

namespace {
volatile uint64_t g_sink;
volatile bool g_keep_going = true;
int recurse(int n) {
  volatile char pad[512];
  pad[0] = static_cast<char>(n);
  return g_keep_going ? recurse(n + 1) + pad[0] : 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  const char* what = argv[1];
  if (!strcmp(what, "null")) {
    g_sink = *reinterpret_cast<volatile uint64_t*>(0);
  } else if (!strcmp(what, "kernel-read")) {
    g_sink = *reinterpret_cast<volatile uint64_t*>(0xFFFFFFFF80000000ull);  // kernel text
  } else if (!strcmp(what, "kernel-write")) {
    *reinterpret_cast<volatile uint64_t*>(0xFFFF800000100000ull) = 1;  // direct map
  } else if (!strcmp(what, "text-write")) {
    *reinterpret_cast<volatile uint8_t*>(reinterpret_cast<uintptr_t>(&recurse)) = 0xCC;
  } else if (!strcmp(what, "rodata-write")) {
    static const char kConst[] = "read only";
    *const_cast<volatile char*>(kConst) = 'X';
  } else if (!strcmp(what, "stack-exec")) {
    volatile uint8_t code[16] = {0xC3};  // ret
    reinterpret_cast<void (*)()>(const_cast<uint8_t*>(code))();
  } else if (!strcmp(what, "heap-exec")) {
    auto* code = static_cast<uint8_t*>(malloc(64));
    code[0] = 0xC3;
    reinterpret_cast<void (*)()>(code)();
  } else if (!strcmp(what, "cli")) {
    asm volatile("cli");
  } else if (!strcmp(what, "hlt")) {
    asm volatile("hlt");
  } else if (!strcmp(what, "port-io")) {
    asm volatile("outb %0, %1" ::"a"(static_cast<uint8_t>(0x10)), "Nd"(static_cast<uint16_t>(0xf4)));
  } else if (!strcmp(what, "cr3")) {
    uint64_t v;
    asm volatile("mov %%cr3, %0" : "=r"(v));
    g_sink = v;
  } else if (!strcmp(what, "sgdt")) {
    uint8_t buf[10];
    asm volatile("sgdt %0" : "=m"(buf));  // UMIP makes this fault
    g_sink = buf[0];
  } else if (!strcmp(what, "stack-overflow")) {
    g_sink = static_cast<uint64_t>(recurse(0));
  } else if (!strcmp(what, "ud")) {
    asm volatile("ud2");
  } else if (!strcmp(what, "div0")) {
    volatile int zero = 0;
    g_sink = static_cast<uint64_t>(10 / zero);
  } else if (!strcmp(what, "noncanonical-syscall")) {
    // A syscall whose return address would be non-canonical is impossible to
    // construct from a static binary at 0x400000; instead pass a
    // non-canonical pointer through every pointer argument.
    int64_t r = write(1, reinterpret_cast<void*>(0x8000000000000000ull), 8);
    return r == -sys::kEFAULT ? 3 : 1;
  } else {
    return 2;
  }
  return 0;  // survived: the kernel let it through
}
