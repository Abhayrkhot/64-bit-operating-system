# Shale design

This document describes how each part works and which invariant it maintains.
Measured numbers are in [RESULTS.md](RESULTS.md).

## Boot

`boot/loader.cpp` is a PE32+ UEFI application built with clang
(`x86_64-unknown-windows`, MS ABI) and `lld-link`. `boot/efi.hpp` declares the
UEFI tables and protocols it needs, written from the specification. No
gnu-efi, no third-party bootloader.

1. Read `\kernel.elf`, `\initrd.tar` and `\cmdline.txt` through
   `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` on the boot volume.
2. Validate the kernel ELF and copy its `PT_LOAD` segments into one physically
   contiguous allocation tagged with an OS-defined memory type, so the kernel
   can tell it apart in the memory map.
3. Build page tables:
   - an identity map plus a higher-half direct map (`0xFFFF800000000000`) of
     `[0, max(4 GiB, top of RAM))`, using 2 MiB pages;
   - the kernel at its link address `0xFFFFFFFF80000000`, using 4 KiB pages
     with the segment permissions (`NX` only after `EFER.NXE` is set).
4. `GetMemoryMap` → `ExitBootServices` (retrying without allocating), convert
   the map into a compact region table, load `CR3`, and jump to the kernel
   with a `BootInfo` pointer (`include/shale/bootinfo.hpp`).

The kernel copies `BootInfo` and builds its own tables, which drop the
identity map so null and low addresses fault. QEMU passes the command line
through `fw_cfg` (`opt/shale/cmdline`), so tests don't need to rebuild the ESP
image.

## Memory

**Physical: buddy allocator** (`lib/mm/buddy.cpp`). Frames are indexed by
physical frame number, so a block's buddy is `frame ^ (1 << order)`, and
alignment in index space is physical alignment. The allocator keeps 12 bytes
of metadata per frame (free-list links, order, state, and a 16-bit owner tag
for the heap) and never touches the memory it manages. That is why the host
benchmark can run the identical code. Orders run 0–10 (4 KiB–4 MiB).
`alloc_exact(n)` splits a 2^k block and frees the unused tail, so a 5-page
request costs 5 pages. Double frees and order mismatches are rejected, and
`check_invariants()` walks every list. Firmware boot-services memory and
loader memory are reclaimed once the kernel runs on its own GDT, IDT and page
tables.

**Heap: slab caches** (`lib/mm/heap.cpp`).
- **Size classes:** 14 classes from 16 B to 2 KiB, plus dedicated caches for
  fixed-size kernel objects (`kcache_create`: threads, processes, files).
- **Slab geometry:** a slab is a naturally aligned 2^order-page block with a
  64-byte header. The order is the smallest that keeps waste at or under 1/16.
- **No per-object header:** `kfree` finds the slab through the frame's owner
  tag and an align-down.
- **Empty slabs:** one per cache is kept, the rest go back to the buddy.
- **Large allocations** (over 2 KiB) are exact page runs.

**Virtual memory** (`kernel/mm/vmm.cpp`).
- **Shared kernel half:** PML4 slots 256–511 are created once at boot, so every
  address space shares them.
- **Direct map:** 1 GiB pages when the CPU supports them, `NX`, global.
- **Kernel image:** W^X — text `R-X`, rodata `R--`, data `RW-`.
- **CPU features:** `CR0.WP`, `EFER.NXE`, `CR4.PGE`, SMEP, SMAP and UMIP are
  enabled when present.
- **Kernel stacks** sit in their own region with an unmapped guard page under
  each one.
- **MMIO** (LAPIC, IOAPIC) is mapped uncached in a separate window.

## Interrupts, time and scheduling

- **Interrupt entry:** a GDT with a TSS and IST stacks (double fault, NMI,
  machine check) and 256 IDT stubs that share one entry path, which swaps GS
  only when coming from ring 3.
- **Exception table:** maps the instructions that are allowed to fault (user
  copies, test probes) to fixup addresses.
- **Interrupt controllers:** the 8259s are remapped and masked. The LAPIC and
  IOAPIC come from the ACPI MADT.
- **Clocks:** TSC and LAPIC-timer frequencies are calibrated against PIT
  channel 2 (median of five 10 ms windows). The scheduler tick is 1 kHz.

**Scheduler** (`kernel/sched/sched.cpp`).
- **Policy:** round-robin with 10 ms slices.
- **Preemption:** the tick decrements the running thread's slice. When it runs
  out, the reschedule happens on the outermost interrupt return, and only if
  the interrupted context had `IF` set, so code holding a spinlock (interrupts
  off) is never preempted.
- **Switch:** `switch.S` pushes the six callee-saved registers, swaps stack
  pointers and returns into the next thread. A first switch returns into a
  trampoline.
- **Around each switch:** the scheduler retargets `TSS.rsp0` and the per-CPU
  syscall stack, switches `CR3` when the address space differs, and saves and
  restores FXSAVE state for threads that run user code. The kernel is built
  without SSE, so kernel threads never need it.
- **Dead threads:** a dead thread's stack is freed by whoever runs next, once
  nothing is still executing on it.
- **Blocking:** wait queues, sleeping (sorted sleep list), a sleeping mutex and
  `join`.

## Processes and system calls

**Programs.** User programs are static ELF files in the initrd (`/bin`) or on
ShaleFS. `lib/elf/validate.cpp` checks every executable before mapping
(overflow-checked arithmetic throughout):
- file ranges inside the file;
- segments inside the user range;
- no segment both writable and executable;
- no two segments sharing a page;
- no dynamic linking;
- the entry point inside an executable segment.

The same validator is fuzzed on the host with libFuzzer under ASan and UBSan.

**Address space.** Program segments sit at 0x400000, followed by the `brk`
heap. Anonymous `mmap` regions start at `0x100000000000` with a guard gap
between them. The 256 KiB stack is under `0x7FFFFFF00000`, with an unmapped
guard below it. A per-process page limit bounds `brk` + `mmap`.

**Syscall entry.**
- **Entry:** `syscall` → `swapgs`, stash the user RSP in the per-CPU block,
  load the kernel stack, and build a frame identical to the interrupt frame.
- **Return:** the fast path is `sysret`, and it is used only when the return
  RIP is a canonical user address. Anything else returns through `iretq`. This
  is the CVE-2012-0217 class of bug.
- **Flags:** RFLAGS is sanitized, and `FMASK` clears IF, TF, DF, AC and NT on
  entry.

**User memory.** The kernel touches user memory only through
`kernel/proc/usercopy`:
1. an overflow-safe range check against the user half;
2. a copy bracketed by `stac`/`clac` (SMAP);
3. exception-table fixups: a fault inside the copy returns `-EFAULT` instead
   of crashing.

Reads and writes go through a kernel bounce buffer, so file backends never see
user pointers.

**Faults** from ring 3 kill only the faulting process:
- page faults, `#GP` (privileged instructions, port I/O, UMIP-blocked `sgdt`)
  and `#SS` → status 128+SIGSEGV;
- `#UD` → 128+SIGILL;
- `#DE` → 128+SIGFPE.

`wait` reaps children. Orphans are reparented to a kernel pseudo-process and
reaped automatically.

## ShaleFS

The on-disk format is in `lib/fs/format.hpp`. The file system (`lib/fs`) is
portable C++ shared by the kernel and the host tools.

```
block 0, 1       superblock slots (written alternately, newest valid wins)
journal          circular log, default 1/16 of the disk (256..8192 blocks)
inode bitmap
block bitmap     metadata regions marked in use
inode table      128-byte inodes: 12 direct, 1 indirect, 1 double-indirect pointer
data             file data, directory blocks (64-byte entries), indirect blocks
```

### Journaling protocol (metadata journaling, ordered data)

Every operation runs in the *running transaction* and first reserves
*credits*: an upper bound on the metadata blocks it can dirty. If the
transaction or the journal can't absorb that, it commits or checkpoints
*before* the operation starts, so a transaction never holds half an
operation.

**Commit** (`fsync`, `sync`, size limit, or the kernel's 1 s commit thread):

1. Write dirty file data in place, then **flush**. Ordered mode: metadata is
   never durable before the data it points at. Data that cache eviction wrote
   back early is covered by the same flush.
2. Write the descriptor block(s) and copies of every dirtied metadata block,
   then a commit block whose **CRC32C covers the whole transaction**. Then
   **flush**. No flush is needed between the copies and the commit block: if
   the device persists the commit block but not everything it covers, the CRC
   fails and recovery ignores the transaction. Without this, each commit would
   need a third flush.
3. The transaction is durable. Its metadata buffers become *checkpoint
   pending*.

**Checkpoint** (journal half full, `sync` under space pressure, unmount):

1. Write checkpoint-pending metadata in place, then **flush**.
2. Advance the journal tail in the next superblock slot, then **flush**.

**Recovery** (every mount). Start at the superblock's tail and sequence
number. A transaction is replayed only if all of these hold: the descriptor
has the expected sequence and the filesystem UUID; every target block is
outside the superblock and journal; the commit block's own CRC is valid; its
block count matches; and the data CRC matches. Replay is in order and stops at
the first transaction that fails. Afterwards the sequence number skips one, so
a half-written transaction left in the log can never look current.

**Invariants the protocol relies on:**

- **Running-transaction metadata stays in cache until commit.** The buffer
  cache never evicts it, because writing it in place early would expose an
  uncommitted state.
- **Freed blocks are not reused until the next checkpoint.** Otherwise a stale
  journaled copy of a former metadata block could be replayed over file data
  that now lives in that block. (ext3 uses revoke records for this; deferring
  reuse is simpler.)
- **Superblock updates alternate slots** with a sequence number and a CRC, so
  a torn superblock write leaves the other slot valid.

### Checking

`lib/fs/fsck.cpp` is independent of `shalefs.cpp`: it shares only the format
header. It checks:
- bitmaps against actual use (inode bits vs. inode types; every data block
  referenced exactly once, and allocated iff referenced);
- block counts;
- nothing mapped past end of file;
- the directory tree (each directory reachable exactly once, parent pointers,
  entry types, valid and unique names);
- exact link counts and orphans.

### Crash testing

`host/tools/crashsim` follows CrashMonkey's approach.
1. **Record:** run a seeded workload on a device that records every block
   write and flush barrier. The workload does writes, appends, overwrites,
   truncates, unlinks, renames (including replacing ones), mkdir/rmdir churn,
   `fsync` and `sync`, plus write-tmp/`fsync`/rename/`fsync` atomic replaces.
2. **Build crash states:** everything before the last completed flush is
   durable; any subset of the writes issued since may be lost or reordered;
   optionally one write is torn at 512-byte granularity.
3. **Check each state:**
   - recovery succeeds;
   - fsck is clean;
   - every file that was `fsync`ed or `sync`ed and not modified afterwards has
     exactly its durable contents;
   - every atomically replaced file holds one complete version, no older than
     the last replace whose `fsync` returned.

Two controls show that the oracle and the generator have teeth:
- the same workload with journaling disabled, metadata written in place on
  commit (`--nojournal`);
- recovery that trusts commit blocks without checking their CRC
  (`--trust-commits`).

`tools/crashtest.py` repeats the experiment end to end. It SIGKILLs QEMU while
`fsstress` runs on the virtio disk. The kernel then recovers the disk on the
next boot, and the host checks fsck and fsstress's durability log.

## Testing

| Layer | What runs | Where |
|---|---|---|
| Host unit tests | buddy and slab against shadow models, ELF validator rules, ShaleFS semantics and model-checked random workloads | `make htest` |
| In-kernel tests | 34 `KTEST`s: boot, allocators, W^X probes, scheduler, user processes, faults, isolation, syscall fuzzing with leak checks, file system | `make ktest` (KVM and TCG) |
| Fuzzing | libFuzzer + ASan/UBSan on the ELF validator; `sysfuzz` hammers every syscall from ring 3 | `make fuzz-elf`, `KTEST user_syscall_fuzz…` |
| Crash consistency | crashsim (states + controls), QEMU kill test | `tools/campaigns.sh` |
| Benchmarks | fragmentation (host), context switch and syscall (kernel and user), file system | `fragbench`, `bench=…`, `run=/bin/fsbench` |
