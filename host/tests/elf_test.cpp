// ELF validator: hand-built images exercising each rejection rule, plus the
// real user programs (all must be accepted).
#include <dirent.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/test.hpp"
#include "elf/validate.hpp"

using namespace shale::elf;

namespace {

constexpr uint64_t kLo = 0x10000, kHi = 0x0000010000000000ull;

struct Builder {
  Ehdr eh{};
  std::vector<Phdr> ph;
  std::vector<uint8_t> payload = std::vector<uint8_t>(8192, 0x90);
  Builder() {
    eh.ident[0] = 0x7f;
    eh.ident[1] = 'E';
    eh.ident[2] = 'L';
    eh.ident[3] = 'F';
    eh.ident[4] = kClass64;
    eh.ident[5] = kData2Lsb;
    eh.type = kTypeExec;
    eh.machine = kMachineX8664;
    eh.version = 1;
    eh.entry = 0x400010;
    eh.phentsize = sizeof(Phdr);
    ph.push_back({kPtLoad, kPfR | kPfX, 4096, 0x400000, 0, 4096, 4096, 4096});
    ph.push_back({kPtLoad, kPfR | kPfW, 8192, 0x401000, 0, 100, 5000, 4096});
  }
  std::vector<uint8_t> build() {
    eh.phoff = sizeof(Ehdr);
    eh.phnum = static_cast<uint16_t>(ph.size());
    std::vector<uint8_t> out(4096 + payload.size(), 0);
    std::memcpy(out.data(), &eh, sizeof(eh));
    std::memcpy(out.data() + sizeof(Ehdr), ph.data(), ph.size() * sizeof(Phdr));
    std::memcpy(out.data() + 4096, payload.data(), payload.size());
    return out;
  }
};

Error check(std::vector<uint8_t> img) {
  Image out;
  return validate(img.data(), img.size(), kLo, kHi, &out);
}

}  // namespace

TEST(elf_accepts_well_formed_image) {
  Builder b;
  auto img = b.build();
  Image out;
  EXPECT(validate(img.data(), img.size(), kLo, kHi, &out) == Error::kOk);
  EXPECT_EQ(out.count, 2u);
  EXPECT_EQ(out.lowest, 0x400000u);
  EXPECT_EQ(out.highest, 0x403000u);  // 0x401000 + 5000 rounded up
}

TEST(elf_rejects_each_violation) {
  {
    Builder b;
    b.eh.ident[1] = 'X';
    EXPECT(check(b.build()) == Error::kBadIdent);
  }
  {
    Builder b;
    b.eh.machine = 3;
    EXPECT(check(b.build()) == Error::kWrongMachine);
  }
  {
    Builder b;
    b.eh.type = 3;  // ET_DYN
    EXPECT(check(b.build()) == Error::kNotExecutable);
  }
  {
    Builder b;
    b.ph[0].flags = kPfR | kPfW | kPfX;
    EXPECT(check(b.build()) == Error::kWriteAndExecute);
  }
  {
    Builder b;
    b.ph[1].vaddr = 0x400800;  // shares a page with the text segment
    EXPECT(check(b.build()) == Error::kOverlap);
  }
  {
    Builder b;
    b.ph[1].filesz = 6000;  // filesz > memsz
    EXPECT(check(b.build()) == Error::kBadSegment);
  }
  {
    Builder b;
    b.ph[0].offset = ~0ull - 10;  // offset + filesz wraps
    EXPECT(check(b.build()) == Error::kBadSegment);
  }
  {
    Builder b;
    b.ph[0].vaddr = ~0ull - 100;  // vaddr + memsz wraps
    EXPECT(check(b.build()) == Error::kOutOfRange);
  }
  {
    Builder b;
    b.ph[0].vaddr = 0xFFFFFFFF80000000ull;  // kernel half
    EXPECT(check(b.build()) == Error::kOutOfRange);
  }
  {
    Builder b;
    b.ph[0].vaddr = 0x1000;  // below the low guard
    EXPECT(check(b.build()) == Error::kOutOfRange);
  }
  {
    Builder b;
    b.eh.entry = 0x401010;  // in the data segment
    EXPECT(check(b.build()) == Error::kBadEntry);
  }
  {
    Builder b;
    b.ph.push_back({3, kPfR, 0, 0, 0, 10, 10, 1});  // PT_INTERP
    EXPECT(check(b.build()) == Error::kDynamic);
  }
  {
    Builder b;
    b.ph.clear();
    b.ph.push_back({6, kPfR, 0, 0, 0, 0, 0, 1});  // only a PT_PHDR
    EXPECT(check(b.build()) == Error::kNoSegments);
  }
  {
    Builder b;
    for (int i = 0; i < 8; i++)
      b.ph.push_back({kPtLoad, kPfR, 4096, 0x500000 + 0x1000ull * i, 0, 16, 16, 4096});
    EXPECT(check(b.build()) == Error::kTooManySegments);
  }
  {
    Builder b;
    auto img = b.build();
    Image out;
    EXPECT(validate(img.data(), 40, kLo, kHi, &out) == Error::kTooSmall);
    // Program headers past the end of a truncated file.
    EXPECT(validate(img.data(), sizeof(Ehdr) + 10, kLo, kHi, &out) == Error::kBadProgramHeaders);
  }
}

TEST(elf_accepts_every_built_user_program) {
  DIR* d = opendir("build/initrd/bin");
  ASSERT(d != nullptr);
  int n = 0;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    std::string path = std::string("build/initrd/bin/") + e->d_name;
    FILE* f = std::fopen(path.c_str(), "rb");
    ASSERT(f != nullptr);
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t r;
    while ((r = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + r);
    std::fclose(f);
    Image out;
    Error err = validate(data.data(), data.size(), kLo, kHi, &out);
    if (err != Error::kOk) std::printf("    %s: %s\n", path.c_str(), error_name(err));
    EXPECT(err == Error::kOk);
    n++;
  }
  closedir(d);
  EXPECT(n >= 5);
}
