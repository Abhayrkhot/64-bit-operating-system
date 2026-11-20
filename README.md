# Shale

Shale is an x86-64 kernel written from scratch in C++17 and assembly. It boots
through its own UEFI loader, which uses no gnu-efi and no third-party
bootloader. It has virtual memory with W^X, a preemptive scheduler, isolated
ring-3 processes, and ShaleFS, a journaling filesystem crash-tested against
recorded and torn-write crash states. Shale runs under QEMU with KVM or TCG.
Every claim below has a test or a benchmark in this repository.

```
shale:/$ ls /bin
-      56024  hello
-      84760  fsstress
...
shale:/$ echo hello from shale > /docs/note.txt
shale:/$ fault null
pid 3 (fault): page fault at 0000000000000000, rip 00000000004001b2: killed
[exit 139]
```

## Results

Measured on QEMU 8.2 (KVM and TCG) on an Intel i7-12700H host. Methodology,
raw data and caveats are in [docs/RESULTS.md](docs/RESULTS.md).

| Area | Result |
|---|---|
| Crash consistency | **0 failures in 97,074 simulated crash states** (10 seeds; lost, reordered and torn writes; 1.59M journal transactions replayed). **200 of 200 QEMU SIGKILLs** during a file-system workload recovered by journal replay, with fsck clean and every `fsync`ed file intact. |
| Controls | Without the journal, 56% of the same crash states fail. If recovery skips the commit CRC, 4.6% fail. |
| Context switch | Voluntary kernel-thread switch: **47 ns** p50 (KVM), 165 ns (TCG). Timer-forced: 1.5 µs end to end, of which Shale's own path is **388 ns**. Process-to-process via syscall + CR3 + FXSAVE: **260 ns**. |
| System calls | `getpid` round trip 65 ns (KVM). 600k fuzzed syscalls from ring 3: 0 crashes, 0 leaked bytes. |
| Page allocator | Buddy leaves 10.8% of free memory unusable for 2 MiB blocks; first-fit leaves 22.4%, so **52% less fragmentation**. |
| Kernel heap | Typed slab caches: **5.2%** overhead vs 9.4% first-fit and 6.3% best-fit. |
| File system | 4 KiB create + `fsync`: 0.96 ms. 256 appends behind one group commit: 1.07 ms, vs 172 ms with an `fsync` each (161×). Sequential writes: 1.5 GiB/s. |
| Hardening | W^X kernel and user mappings, SMEP/SMAP/UMIP, guard pages, `sysret` canonical-RIP check, checked user copies, ELF validator fuzzed (27.7M executions) |

## What's inside

| Area | Implementation |
|---|---|
| Boot | PE32+ UEFI loader written against the spec (`boot/`). It loads the kernel ELF, initrd and command line, builds an identity + higher-half direct map, maps kernel segments with their ELF permissions, exits boot services and hands off a `BootInfo`. |
| Memory | A buddy allocator over all RAM with exact-size runs. Slab caches (14 size classes plus typed caches). 4-level paging with W^X kernel sections, SMEP/SMAP/UMIP, guard-paged kernel stacks and a shared kernel half for user address spaces. |
| Scheduling | APIC timer calibrated against the PIT, 1 kHz tick, preemptive round-robin, an assembly context switch, lazy FXSAVE for user threads, sleep, wait queues, mutexes, join. |
| Processes | ELF loading behind a validator (fuzzed), `syscall`/`sysret` with a canonical-RIP check, 26 system calls, user pointers checked by range, SMAP-bracketed copies and exception-table fixups. Faults kill only the faulting process. |
| Storage | PCI enumeration, a legacy virtio-blk driver (async bounce-buffered writes, batched doorbell, FLUSH ordering), a VFS with the initrd at `/bin` and ShaleFS at `/`. |
| ShaleFS | Inodes with direct/indirect/double-indirect blocks, bitmaps, directories. Metadata journaling in ordered mode with CRC32C-checksummed commits, alternating superblocks, deferred block reuse, read-ahead and an independent fsck. |
| User space | crt0 + a small libc, a shell, and test, stress, fuzz and benchmark programs. |

How each part works, and which invariant it maintains: [docs/DESIGN.md](docs/DESIGN.md).

## Build and run

Everything builds and runs inside a Docker image (clang/lld 18, QEMU, OVMF):

```bash
scripts/dev.sh 'make -j all'                  # loader, kernel, user programs, ESP image, host tools
```

```bash
scripts/dev.sh 'make test'                    # host unit tests + all in-kernel tests
```

```bash
scripts/dev.sh 'make run'                     # boot to the shell (serial console)
```

```bash
scripts/dev.sh 'tools/campaigns.sh'           # crash-consistency campaigns
```

```bash
scripts/dev.sh 'python3 tools/bench.py'       # benchmark medians
```

`tools/qemu.py` boots the ESP image under OVMF. It passes the kernel command
line through fw_cfg and maps the kernel's `isa-debug-exit` status to
pass/fail/panic. Boot modes: `test=<prefix|all>`, `bench=<name>`,
`run=/bin/prog args=a:b`, `fsck`, or (default) the shell.

## Layout

```
boot/            UEFI loader
include/shale/   boot protocol, ELF and syscall ABI shared by loader, kernel, user space
kernel/          arch/x86_64, mm, sched, proc, fs, drivers, bench, test
lib/             portable code built into both the kernel and host tools:
                 mm (buddy, slab), fs (ShaleFS, fsck), elf (validator), fmt, util
user/            libc and programs (sh, fstest, fsstress, sysfuzz, usertest, ...)
host/            unit tests, fuzz harness, tools: crashsim, fragbench, shalefs
tools/           QEMU runner, crash test, benchmark and campaign drivers
docs/            DESIGN.md, RESULTS.md
```

## Limitations

- One CPU. Locks are written to be SMP-safe, but there is no AP bring-up.
- Disk completion is polled rather than interrupt-driven.
- Processes are single-threaded. There is no `fork`; processes are created
  with `spawn`.
- There are no signals beyond fault kills and `kill` of a child.
- ShaleFS has no hard links or symlinks. Directories are linear lists.
- Under TCG, a ring 3 ↔ ring 0 round trip costs about 16 µs. That is QEMU's
  emulation cost: the same path is 50–65 ns under KVM (see RESULTS).
