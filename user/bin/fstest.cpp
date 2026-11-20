// File-system system calls from user space: semantics and error cases.
#include "shale.hpp"

namespace {
int g_failures;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      printf("fstest: FAILED %s (line %d)\n", #cond, __LINE__);        \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

int write_all(const char* path, const void* data, uint64_t n, uint64_t flags = 0) {
  int fd = open(path, sys::kOWrite | sys::kOCreate | sys::kOTruncate | flags);
  if (fd < 0) return fd;
  int64_t w = write(fd, data, n);
  close(fd);
  return w == static_cast<int64_t>(n) ? 0 : -1;
}

void basic() {
  CHECK(write_all("/note.txt", "hello shalefs\n", 14) == 0);
  int fd = open("/note.txt", sys::kOReadWrite);
  CHECK(fd >= 0);
  char buf[64] = {};
  CHECK(read(fd, buf, sizeof(buf)) == 14);
  CHECK(!memcmp(buf, "hello shalefs\n", 14));
  CHECK(lseek(fd, 6, sys::kSeekSet) == 6);
  CHECK(write(fd, "SHALE", 5) == 5);
  CHECK(lseek(fd, 0, sys::kSeekEnd) == 14);
  CHECK(lseek(fd, -20, sys::kSeekCur) == -sys::kEINVAL);
  close(fd);
  fd = open("/note.txt", sys::kOWrite | sys::kOAppend);
  CHECK(write(fd, "more\n", 5) == 5);
  CHECK(fsync(fd) == 0);
  close(fd);
  sys::Stat st;
  CHECK(stat("/note.txt", &st) == 0 && st.size == 19 && st.type == sys::kTypeFile);
  fd = open("/note.txt", 0);
  read(fd, buf, sizeof(buf));
  CHECK(!memcmp(buf, "hello SHALEfs\nmore\n", 19));
  close(fd);
  CHECK(truncate("/note.txt", 5) == 0);
  CHECK(stat("/note.txt", &st) == 0 && st.size == 5);
}

void directories() {
  CHECK(mkdir("/dir") == 0);
  CHECK(mkdir("/dir") == -sys::kEEXIST);
  CHECK(mkdir("/dir/sub") == 0);
  CHECK(mkdir("/missing/sub") == -sys::kENOENT);
  CHECK(write_all("/dir/sub/a", "a", 1) == 0);
  CHECK(unlink("/dir/sub") == -sys::kENOTEMPTY);
  CHECK(open("/dir", sys::kOWrite) == -sys::kEISDIR);
  CHECK(open("/note.txt", sys::kODirectory) == -sys::kENOTDIR);
  CHECK(open("/note.txt/x", 0) == -sys::kENOTDIR);
  for (int i = 0; i < 200; i++) {
    char p[32];
    snprintf(p, sizeof(p), "/dir/f%03d", i);
    CHECK(write_all(p, p, strlen(p)) == 0);
  }
  int fd = open("/dir", 0);
  sys::DirEntry e;
  int n = 0;
  while (readdir(fd, &e, static_cast<uint64_t>(n)) == 1) n++;
  close(fd);
  CHECK(n == 201);  // 200 files + sub
  CHECK(rename("/dir/f000", "/dir/sub/moved") == 0);
  CHECK(rename("/dir/f001", "/dir/f002") == 0);  // replace
  char buf[16] = {};
  fd = open("/dir/f002", 0);
  read(fd, buf, sizeof(buf));
  close(fd);
  CHECK(!strcmp(buf, "/dir/f001"));
  CHECK(rename("/dir", "/dir/sub/inside") == -sys::kEINVAL);
  CHECK(unlink("/dir/sub/a") == 0 && unlink("/dir/sub/moved") == 0 && unlink("/dir/sub") == 0);
  fd = open("/", 0);
  bool saw_bin = false;
  for (int i = 0; readdir(fd, &e, static_cast<uint64_t>(i)) == 1; i++)
    if (!strcmp(e.name, "bin")) saw_bin = true;
  close(fd);
  CHECK(saw_bin);
}

void big_file() {
  constexpr uint64_t kSize = 5 << 20;
  auto* data = static_cast<uint8_t*>(malloc(kSize));
  for (uint64_t i = 0; i < kSize; i++) data[i] = static_cast<uint8_t>(i * 2654435761u >> 13);
  uint64_t t0 = clock_ns();
  CHECK(write_all("/big.bin", data, kSize) == 0);
  int fd = open("/big.bin", 0);
  CHECK(fsync(fd) == 0);
  uint64_t t1 = clock_ns();
  auto* back = static_cast<uint8_t*>(malloc(kSize));
  CHECK(read(fd, back, kSize) == static_cast<int64_t>(kSize));
  uint64_t t2 = clock_ns();
  close(fd);
  CHECK(!memcmp(data, back, kSize));
  printf("fstest: 5 MiB write+fsync %lu ms, read %lu ms\n", (t1 - t0) / 1000000, (t2 - t1) / 1000000);
  CHECK(unlink("/big.bin") == 0);
  free(data);
  free(back);
}

void exec_from_disk() {
  // Copy a program out of the initrd onto ShaleFS and run it from there.
  int in = open("/bin/hello", 0);
  sys::Stat st;
  stat("/bin/hello", &st);
  auto* img = static_cast<uint8_t*>(malloc(st.size));
  CHECK(read(in, img, st.size) == static_cast<int64_t>(st.size));
  close(in);
  CHECK(write_all("/hello-copy", img, st.size) == 0);
  free(img);
  const char* args[] = {"hello-copy", "9", nullptr};
  int pid = spawn("/hello-copy", args);
  CHECK(pid > 0);
  int status = -1;
  wait(pid, &status);
  CHECK(status == 9);
  CHECK(unlink("/hello-copy") == 0);
}
}  // namespace

int main(int, char**) {
  basic();
  directories();
  big_file();
  exec_from_disk();
  CHECK(sync() == 0);
  printf("fstest: %s (%d failure%s)\n", g_failures ? "FAILED" : "ok", g_failures, g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
