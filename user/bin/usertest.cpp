// User-side checks of the system call layer, especially pointer validation:
// every bad pointer must come back as -EFAULT, never crash the kernel and
// never write outside the caller's buffer.
#include "libc.hpp"

namespace {
int g_failures;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      printf("usertest: FAILED %s (line %d)\n", #cond, __LINE__);       \
      g_failures++;                                                     \
    }                                                                   \
  } while (0)

const uint64_t kKernelAddrs[] = {0xFFFFFFFF80000000ull, 0xFFFF800000000000ull, 0xFFFF800000100000ull,
                                 0x0000800000000000ull, 0x8000000000000000ull, 0xFFFFFFFFFFFFF000ull};

void test_write_rejects_bad_pointers() {
  for (uint64_t a : kKernelAddrs) CHECK(write(1, reinterpret_cast<void*>(a), 16) == -sys::kEFAULT);
  CHECK(write(1, reinterpret_cast<void*>(0x1000), 16) == -sys::kEFAULT);  // unmapped
  // Range that starts in user space and wraps or crosses into the kernel half.
  CHECK(write(1, reinterpret_cast<void*>(0x00007FFFFFFFF000ull), 0x2000) == -sys::kEFAULT);
  CHECK(write(1, reinterpret_cast<void*>(0x1000), ~0ull) == -sys::kEINVAL);
  CHECK(write(99, "x", 1) == -sys::kEBADF);
  CHECK(write(-1, "x", 1) == -sys::kEBADF);
}

void test_read_into_bad_or_readonly_buffers() {
  int fd = open("/bin/hello", sys::kORead);
  CHECK(fd >= 0);
  for (uint64_t a : kKernelAddrs) CHECK(read(fd, reinterpret_cast<void*>(a), 16) == -sys::kEFAULT);
  // Read-only user page (our own text): the kernel's copy faults and fixes up.
  CHECK(read(fd, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&test_write_rejects_bad_pointers) & ~0xFFFull), 16) == -sys::kEFAULT);
  // A valid buffer gets the ELF magic, and only 4 bytes are written.
  char buf[8] = {0, 0, 0, 0, 'z', 'z', 'z', 'z'};
  CHECK(lseek(fd, 0, sys::kSeekSet) == 0);
  CHECK(read(fd, buf, 4) == 4);
  CHECK(buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F' && buf[4] == 'z');
  CHECK(close(fd) == 0);
  CHECK(close(fd) == -sys::kEBADF);
}

void test_paths() {
  CHECK(open(reinterpret_cast<const char*>(0xFFFFFFFF80000000ull), 0) == -sys::kEFAULT);
  CHECK(open(reinterpret_cast<const char*>(0x1000), 0) == -sys::kEFAULT);
  char long_path[400];
  memset(long_path, 'a', sizeof(long_path) - 1);
  long_path[0] = '/';
  long_path[sizeof(long_path) - 1] = 0;
  CHECK(open(long_path, 0) == -sys::kENAMETOOLONG);
  CHECK(open("/bin/missing", 0) == -sys::kENOENT);
  CHECK(open("/bin/hello", sys::kOWrite) == -sys::kEROFS);
  // A path string running into an unmapped page without a NUL.
  int64_t m = mmap(4096, sys::kProtRead | sys::kProtWrite);
  CHECK(m > 0);
  memset(reinterpret_cast<void*>(m), 'b', 4096);
  CHECK(open(reinterpret_cast<const char*>(m), 0) == -sys::kENAMETOOLONG ||
        open(reinterpret_cast<const char*>(m), 0) == -sys::kEFAULT);
  CHECK(open(reinterpret_cast<const char*>(m + 4000), 0) == -sys::kEFAULT);
  CHECK(munmap(static_cast<uint64_t>(m), 4096) == 0);
}

void test_memory_syscalls() {
  uint64_t start = brk(0);
  CHECK(start > 0x400000);
  CHECK(brk(start + 100000) == start + 100000);
  memset(reinterpret_cast<void*>(start), 0x77, 100000);
  CHECK(brk(start) == start);
  CHECK(brk(start - 4096) == start);          // below the heap start: refused
  CHECK(brk(start + (1ull << 40)) == start);  // too large: refused
  int64_t a = mmap(8192, sys::kProtRead | sys::kProtWrite);
  CHECK(a > 0);
  memset(reinterpret_cast<void*>(a), 1, 8192);
  CHECK(munmap(static_cast<uint64_t>(a) + 4096, 4096) == -sys::kEINVAL);  // partial: refused
  CHECK(munmap(0x400000, 4096) == -sys::kEINVAL);  // our text: refused
  CHECK(munmap(static_cast<uint64_t>(a), 8192) == 0);
  CHECK(mmap(0, 3) == -sys::kEINVAL);
  CHECK(mmap(1ull << 40, 3) == -sys::kEINVAL);
  // Read-only mapping really is read-only to the kernel too.
  int64_t ro = mmap(4096, sys::kProtRead);
  int fd = open("/bin/hello", 0);
  CHECK(read(fd, reinterpret_cast<void*>(ro), 16) == -sys::kEFAULT);
  close(fd);
  // malloc stress
  void* ptrs[256] = {};
  for (int round = 0; round < 20; round++) {
    for (int i = 0; i < 256; i++) {
      size_t n = 1 + static_cast<size_t>((i * 7919 + round * 104729) % 9000);
      free(ptrs[i]);
      ptrs[i] = malloc(n);
      CHECK(ptrs[i] != nullptr);
      memset(ptrs[i], i, n);
    }
  }
  for (void* p : ptrs) free(p);
}

void test_processes() {
  const char* args[] = {"hello", "42", nullptr};
  int pid = spawn("/bin/hello", args);
  CHECK(pid > 0);
  int status = -1;
  CHECK(wait(pid, &status) == pid);
  CHECK(status == 42);
  CHECK(wait(-1, &status) == -sys::kECHILD);
  CHECK(wait(pid, reinterpret_cast<int*>(0xFFFF800000000000ull)) == -sys::kEFAULT);
  CHECK(spawn("/bin/nope", nullptr) == -sys::kENOENT);
  CHECK(spawn(reinterpret_cast<const char*>(0xFFFFFFFF80000000ull), nullptr) == -sys::kEFAULT);
  const char* bad_argv[] = {"hello", reinterpret_cast<const char*>(0xFFFF800000000000ull), nullptr};
  CHECK(spawn("/bin/hello", bad_argv) == -sys::kEFAULT);
  CHECK(spawn("/bin/hello", reinterpret_cast<const char* const*>(0x1000)) == -sys::kEFAULT);
  CHECK(kill(12345) == -sys::kESRCH);
}

void test_directory_and_stat() {
  int fd = open("/bin", sys::kORead);
  CHECK(fd >= 0);
  sys::DirEntry e;
  int n = 0;
  bool saw_hello = false;
  while (readdir(fd, &e, static_cast<uint64_t>(n)) == 1) {
    if (!strcmp(e.name, "hello")) saw_hello = true;
    n++;
  }
  CHECK(n >= 5 && saw_hello);
  CHECK(readdir(fd, reinterpret_cast<sys::DirEntry*>(0xFFFF800000000000ull), 0) == -sys::kEFAULT);
  close(fd);
  sys::Stat st;
  CHECK(stat("/bin/hello", &st) == 0 && st.size > 1000 && st.type == sys::kTypeFile);
  CHECK(stat("/bin/hello", reinterpret_cast<sys::Stat*>(0xFFFFFFFF80000000ull)) == -sys::kEFAULT);
}

void test_time() {
  uint64_t t0 = clock_ns();
  sleep_ms(10);
  uint64_t dt = clock_ns() - t0;
  CHECK(dt >= 10000000 && dt < 100000000);
  CHECK(syscall6(9999) == -sys::kENOSYS);
}
}  // namespace

int main(int, char**) {
  test_write_rejects_bad_pointers();
  test_read_into_bad_or_readonly_buffers();
  test_paths();
  test_memory_syscalls();
  test_processes();
  test_directory_and_stat();
  test_time();
  printf("usertest: %s (%d failure%s)\n", g_failures ? "FAILED" : "ok", g_failures, g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
