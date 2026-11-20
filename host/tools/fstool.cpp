// fstool: host tool for file-system disk images.
//
//   fstool mkfs <image> <MiB> [--nojournal]
//   fstool fsck <image>            recover (replay the journal), then check
//   fstool ls <image> <dir>
//   fstool cat <image> <file>
//   fstool crc <image> <file>      prints "<len> <crc32c>" or "missing"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/blockdevs.hpp"
#include "fs/fsck.hpp"
#include "fs/filesystem.hpp"
#include "util/crc32c.hpp"

using os::fs::InodeInfo;
using os::fs::InodeType;
using os::fs::FileSystem;

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: fstool mkfs <image> <MiB> [--nojournal]\n"
               "       fstool fsck|ls|cat|crc <image> [path]\n");
  return 2;
}

bool read_file(FileSystem& fs, const char* path, std::string* out) {
  uint32_t ino;
  if (fs.lookup(path, &ino) != 0) return false;
  InodeInfo info;
  if (fs.stat(ino, &info) != 0 || info.type != InodeType::kFile) return false;
  out->assign(info.size, '\0');
  return info.size == 0 || fs.read(ino, 0, &(*out)[0], info.size) == static_cast<int64_t>(info.size);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return usage();
  std::string cmd = argv[1], image = argv[2];
  host::FileDevice dev;
  if (cmd == "mkfs") {
    if (argc < 4) return usage();
    uint64_t mib = std::strtoull(argv[3], nullptr, 10);
    if (dev.open(image, true, mib << 20) != 0) {
      std::perror(image.c_str());
      return 1;
    }
    FileSystem::MkfsOptions o;
    o.journaling = !(argc > 4 && !std::strcmp(argv[4], "--nojournal"));
    o.uuid = 0x5348414c45ull ^ (mib << 32);
    int r = FileSystem::mkfs(&dev, o);
    std::printf("mkfs: %s (%lu MiB, %s): %s\n", image.c_str(), static_cast<unsigned long>(mib),
                o.journaling ? "journaled" : "no journal", r == 0 ? "ok" : "failed");
    return r == 0 ? 0 : 1;
  }
  if (dev.open(image, false) != 0) {
    std::perror(image.c_str());
    return 1;
  }
  FileSystem fs;
  FileSystem::Options o;
  o.cache_blocks = 1024;
  if (cmd == "fsck") {
    if (fs.mount(&dev, o) != 0) {
      std::printf("fsck: mount (recovery) failed\n");
      return 1;
    }
    uint64_t replayed = fs.stats().replayed_txns;
    fs.unmount();
    os::fs::FsckReport rep;
    os::fs::fsck(&dev, &rep);
    std::printf("fsck: %u error(s), %u files, %u dirs, %lu bytes in files, replayed %lu txns\n", rep.errors,
                rep.files, rep.dirs, static_cast<unsigned long>(rep.file_bytes), static_cast<unsigned long>(replayed));
    for (uint32_t i = 0; i < rep.errors && i < os::fs::FsckReport::kMaxMessages; i++)
      std::printf("  %s\n", rep.messages[i]);
    return rep.errors ? 1 : 0;
  }
  if (argc < 4) return usage();
  if (fs.mount(&dev, o) != 0) {
    std::fprintf(stderr, "mount failed\n");
    return 1;
  }
  const char* path = argv[3];
  int status = 0;
  if (cmd == "ls") {
    uint32_t dir;
    if (fs.lookup(path, &dir) != 0) {
      std::fprintf(stderr, "%s: not found\n", path);
      status = 1;
    } else {
      char name[58];
      uint32_t child;
      InodeType type;
      for (uint64_t i = 0; fs.readdir(dir, i, name, &child, &type) == 1; i++) {
        InodeInfo info{};
        fs.stat(child, &info);
        std::printf("%c %10lu %s\n", type == InodeType::kDir ? 'd' : '-', static_cast<unsigned long>(info.size), name);
      }
    }
  } else if (cmd == "cat") {
    std::string data;
    if (!read_file(fs, path, &data)) {
      std::fprintf(stderr, "%s: not found\n", path);
      status = 1;
    } else {
      std::fwrite(data.data(), 1, data.size(), stdout);
    }
  } else if (cmd == "crc") {
    std::string data;
    if (!read_file(fs, path, &data))
      std::printf("missing\n");
    else
      std::printf("%zu %u\n", data.size(), os::crc32c(data.data(), data.size()));
  } else {
    status = usage();
  }
  fs.unmount();
  return status;
}
