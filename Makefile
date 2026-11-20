# Shale build. Run inside the toolchain container: scripts/dev.sh 'make -j test'
#
#   make            loader, kernel, user programs, ESP image, host tools
#   make ktest      boot the kernel in QEMU and run the in-kernel tests
#   make htest      host unit tests (allocators, filesystem, ELF validation)
#   make test       both
#   make run        boot to the shell on the serial console

BUILD ?= build
CXX := clang++
CC := clang
RESDIR := $(shell $(CXX) -print-resource-dir)
QEMU_RUN := python3 tools/qemu.py

WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
FREESTANDING := -ffreestanding -nostdinc -isystem $(RESDIR)/include -fno-exceptions -fno-rtti \
  -fno-threadsafe-statics -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables

# --- UEFI loader: PE32+ application, Microsoft x64 ABI -----------------------
BOOT_CXXFLAGS := --target=x86_64-unknown-windows -std=c++17 $(FREESTANDING) -fshort-wchar -mno-red-zone \
  -mno-stack-arg-probe -mno-mmx -mno-sse -mno-sse2 -O2 $(WARN) -Iinclude
BOOT_OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(wildcard boot/*.cpp))

$(BUILD)/boot/%.o: boot/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(BOOT_CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/BOOTX64.EFI: $(BOOT_OBJS)
	lld-link -subsystem:efi_application -entry:efi_main -nodefaultlib -out:$@ $^

# --- Kernel: freestanding ELF, higher half, no SSE --------------------------
KERNEL_CXXFLAGS := --target=x86_64-unknown-none-elf -std=c++17 $(FREESTANDING) -mno-red-zone -mcmodel=kernel \
  -mno-mmx -mno-sse -mno-sse2 -mno-80387 -fno-pic -fno-pie -fno-omit-frame-pointer -O2 -g $(WARN) \
  -Iinclude -Ikernel -Ilib -DSHALE_KERNEL=1
KERNEL_ASFLAGS := --target=x86_64-unknown-none-elf -Iinclude -Ikernel -g
# Portable libraries compiled into the kernel (also built for host tests).
SHARED_LIB_DIRS := lib/fmt lib/mm lib/fs lib/util lib/elf
SHARED_SRCS := $(wildcard $(addsuffix /*.cpp,$(SHARED_LIB_DIRS)))
FREESTANDING_SRCS := $(wildcard lib/freestanding/*.cpp)
KERNEL_SRCS := $(shell find kernel -name '*.cpp' -o -name '*.S' | sort) $(SHARED_SRCS) $(FREESTANDING_SRCS)
KERNEL_OBJS := $(patsubst %,$(BUILD)/k/%.o,$(KERNEL_SRCS))

$(BUILD)/k/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(KERNEL_CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/k/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_ASFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kernel.elf: $(KERNEL_OBJS) kernel/linker.ld
	ld.lld -T kernel/linker.ld -nostdlib -static -z max-page-size=4096 -o $@ $(KERNEL_OBJS)

$(BUILD)/kernel.stripped: $(BUILD)/kernel.elf
	llvm-strip -o $@ $<

# --- User programs ----------------------------------------------------------
USER_CXXFLAGS := --target=x86_64-unknown-none-elf -std=c++17 $(FREESTANDING) -fno-pic -fno-pie \
  -fno-omit-frame-pointer -O2 -g $(WARN) -Iinclude -Iuser/libc -Ilib -DSHALE_USER=1
USER_LIBC_SRCS := $(wildcard user/libc/*.cpp user/libc/*.S) lib/fmt/format.cpp lib/util/crc32c.cpp $(FREESTANDING_SRCS)
USER_LIBC_OBJS := $(patsubst %,$(BUILD)/u/%.o,$(USER_LIBC_SRCS))
USER_PROGS := $(notdir $(basename $(wildcard user/bin/*.cpp)))
USER_ELFS := $(patsubst %,$(BUILD)/initrd/bin/%,$(USER_PROGS))

$(BUILD)/u/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(USER_CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/u/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) --target=x86_64-unknown-none-elf -g -c $< -o $@

$(BUILD)/initrd/bin/%: $(BUILD)/u/user/bin/%.cpp.o $(USER_LIBC_OBJS) user/user.ld
	@mkdir -p $(dir $@)
	ld.lld -T user/user.ld -nostdlib -static -z max-page-size=4096 -o $@ $< $(USER_LIBC_OBJS)

$(BUILD)/initrd.tar: $(USER_ELFS) $(wildcard initrd/*)
	@mkdir -p $(BUILD)/initrd
	cp -r initrd/. $(BUILD)/initrd/ 2>/dev/null || true
	tar --format=ustar --owner=0 --group=0 --sort=name -C $(BUILD)/initrd -cf $@ .

# --- Boot image -------------------------------------------------------------
$(BUILD)/esp.img: $(BUILD)/BOOTX64.EFI $(BUILD)/kernel.stripped $(BUILD)/initrd.tar
	rm -f $@
	truncate -s 32M $@
	mkfs.fat -F 16 -n SHALE $@ >/dev/null
	mmd -i $@ ::/EFI ::/EFI/BOOT
	mcopy -i $@ $(BUILD)/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ $(BUILD)/kernel.stripped ::/kernel.elf
	mcopy -i $@ $(BUILD)/initrd.tar ::/initrd.tar

# --- Host tools and tests ---------------------------------------------------
HOST_CXXFLAGS := -std=c++17 -O2 -g $(WARN) -Iinclude -Ilib -Ihost -DSHALE_HOSTED=1
HOST_LIB_OBJS := $(patsubst %,$(BUILD)/h/%.o,$(SHARED_SRCS))
HOST_TEST_SRCS := $(wildcard host/tests/*.cpp)
HOST_TOOLS := $(patsubst host/tools/%.cpp,$(BUILD)/host/%,$(wildcard host/tools/*.cpp))
HOST_COMMON_SRCS := $(wildcard host/common/*.cpp)
HOST_COMMON_OBJS := $(patsubst %,$(BUILD)/h/%.o,$(HOST_COMMON_SRCS))

$(BUILD)/h/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/host/unit_tests: $(patsubst %,$(BUILD)/h/%.o,$(HOST_TEST_SRCS)) $(HOST_LIB_OBJS) $(HOST_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $^

$(BUILD)/host/%: $(BUILD)/h/host/tools/%.cpp.o $(HOST_LIB_OBJS) $(HOST_COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $^

# --- Top-level targets ------------------------------------------------------
.PHONY: all image host ktest htest test run clean
.DEFAULT_GOAL := all
all: image host
image: $(BUILD)/esp.img $(BUILD)/kernel.elf
host: $(HOST_TOOLS) $(if $(HOST_TEST_SRCS),$(BUILD)/host/unit_tests)

ktest: image
	$(QEMU_RUN) --cmdline "test=$(or $(FILTER),all)" --data-disk --timeout 300

htest: $(if $(HOST_TEST_SRCS),$(BUILD)/host/unit_tests)
	$(if $(HOST_TEST_SRCS),$(BUILD)/host/unit_tests)

test: htest ktest

run: image
	$(QEMU_RUN) --interactive --data-disk

# libFuzzer + ASan/UBSan over the ELF validator, seeded with the real programs.
FUZZ_SECONDS ?= 60
FUZZ_FLAGS := -std=c++17 -O1 -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
  -Iinclude -Ilib -DSHALE_HOSTED=1
$(BUILD)/fuzz/elf_fuzz: host/fuzz/elf_fuzz.cpp lib/elf/validate.cpp lib/elf/validate.hpp
	@mkdir -p $(dir $@)
	$(CXX) $(FUZZ_FLAGS) -o $@ host/fuzz/elf_fuzz.cpp lib/elf/validate.cpp

.PHONY: fuzz-elf
fuzz-elf: $(BUILD)/fuzz/elf_fuzz $(USER_ELFS)
	mkdir -p $(BUILD)/fuzz/elf_corpus
	cp $(USER_ELFS) $(BUILD)/fuzz/elf_corpus/
	$(BUILD)/fuzz/elf_fuzz -max_total_time=$(FUZZ_SECONDS) -max_len=65536 -print_final_stats=1 \
	  $(BUILD)/fuzz/elf_corpus

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
