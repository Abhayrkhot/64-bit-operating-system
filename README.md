# Shale

Shale is a small x86-64 kernel written from scratch in C++17 and assembly. It
boots through its own UEFI loader, which uses no gnu-efi and no third-party
bootloader. It is built and tested under QEMU, with KVM and with TCG.

Status: work in progress. Each milestone lands with its tests.

| Milestone | Contents |
|---|---|
| M0 | UEFI loader (PE32+, written against the spec), higher-half kernel, serial console, in-kernel test runner |
| M1 | Physical (buddy) and slab allocators, kernel page tables, fragmentation benchmark |
| M2 | Interrupts, APIC timer, preemptive scheduler, context-switch benchmark |
| M3 | Ring 3 processes, `syscall`/`sysret`, ELF loader, user-pointer validation, syscall fuzzer |
| M4 | virtio-blk, buffer cache, journaling filesystem, crash-consistency harness |

## Build and run

Everything builds inside a Docker image (clang/lld 18, QEMU, OVMF):

```bash
scripts/dev.sh 'make -j test'      # build, then run host and in-kernel tests
scripts/dev.sh 'make run'          # boot with the serial console attached
```

`tools/qemu.py` boots the ESP image under OVMF. It passes the kernel command
line through QEMU's fw_cfg and maps the kernel's exit status from the
`isa-debug-exit` device to pass, fail or panic.

## Layout

```
boot/          UEFI loader (efi.hpp: hand-written UEFI types; loader.cpp)
include/shale/ boot protocol and ELF definitions shared by loader, kernel, tools
kernel/        the kernel (arch/x86_64, core, mm, sched, fs, drivers, test)
lib/           portable code compiled into both the kernel and the host tests
host/          host-side tools (mkfs, fsck, crash simulator) and unit tests
user/          user-space libc and programs, packed into the initrd
tools/         QEMU runner and scripts
```
