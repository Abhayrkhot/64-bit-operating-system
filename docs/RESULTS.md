# Shale: measured results

All numbers come from the code in this repository, run with the commands
shown. Each table reports medians across runs or seeds, never the best run.

**Environment.** Intel Core i7-12700H laptop (20 threads, 61 GB RAM), Ubuntu
26.04 host. Toolchain image `docker/dev.Dockerfile` (Ubuntu 24.04: clang/lld
18, QEMU 8.2.2, OVMF 2024.02). Guests get 1 vCPU and 512 MiB. "KVM" means
`-accel kvm -cpu host`; "TCG" means QEMU's emulator with `-cpu max`. Virtio
disks are raw image files with `cache=writeback`, so a guest flush becomes a
host `fdatasync` on NVMe. The host is shared with other workloads, which is
why medians are reported. CI also runs the test suite on GitHub's AMD EPYC
runners under KVM and TCG.

Reproduce everything with:

```bash
scripts/dev.sh 'make -j all && python3 tools/bench.py --runs 5 && tools/campaigns.sh'
```

## Crash consistency (ShaleFS)

### Simulated crashes: `host/tools/crashsim`

Each seed runs a 1,500-operation workload: writes, appends, overwrites,
truncates, unlinks, renames (including replacing ones), mkdir/rmdir churn,
`fsync`, `sync`, and write-tmp/`fsync`/rename/`fsync` atomic replaces. Every
block write and flush is recorded. Crash states keep every write before the
last completed flush and an arbitrary subset of the writes issued after it.
In torn mode, some of those writes are also cut at a 512-byte sector
boundary.

A state fails if any of these happen:
- recovery fails;
- fsck finds any inconsistency;
- a file that was `fsync`ed or `sync`ed and not modified afterwards has
  different contents;
- an atomically replaced file holds something other than one complete
  version no older than the last durable replace.

10 seeds (`tools/campaigns.sh`):

| Configuration | Crash states | Transactions replayed | Failed states | fsck failures | Durability violations | Atomic-replace violations |
|---|---|---|---|---|---|---|
| **ShaleFS (journal, torn writes)** | **97,074** | 1,589,369 | **0** | 0 | 0 | 0 |
| Control: no journal (metadata written in place on commit) | 98,665 | 0 | 55,606 (56.4%) | 48,997 | 0 | 6,609 |
| Control: recovery ignores commit-block CRC, torn writes | 97,074 | 1,590,432 | 4,499 (4.6%) | 1,063 | 974 | 2,462 |

The controls show that the oracle and the crash-state generator detect real
damage:
- **Without the journal,** half the crash points leave the metadata
  inconsistent (bitmaps vs. inodes, orphaned inodes, entries naming freed
  inodes).
- **When recovery trusts commit blocks without the CRC,** it replays
  transactions whose blocks never reached the disk. The commit checksum is
  what lets ShaleFS write a transaction's descriptor, metadata copies and
  commit block behind a single flush.

### Real crashes: `tools/crashtest.py`

QEMU is SIGKILLed at a random point (0.05–2 s) while `/bin/fsstress` runs on a
fresh 64 MiB virtio disk. Shale then boots again, recovers the disk (journal
replay) and runs fsck in the kernel. The host runs fsck again and checks
fsstress's durability log line by line (every `DURABLE` file and every
`REPLACE`) against the recovered files.

| Trials | Killed mid-workload | Kill time (min / median / max) | Journal transactions replayed at reboot | Recovered-file checks | fsck errors (kernel or host) | Durability violations |
|---|---|---|---|---|---|---|
| **200** | 200 | 0.07 / 1.04 / 1.97 s | 7,294 | 2,704 | **0** | **0** |

An earlier 200-trial campaign, on the kernel before read-ahead and doorbell
batching, gave the same result: 0 / 0, 7,738 transactions replayed.

## Context switches and system calls

`bench=ctxswitch` and `bench=process_switch`, 5 boots each, median of the
per-boot medians (`tools/bench.py`).

| Measurement | KVM p50 | KVM p99 | TCG p50 | TCG p99 |
|---|---|---|---|---|
| Kernel thread → kernel thread, `yield()` (200k switches) | **47 ns** | 62 ns | 165 ns | 290 ns |
| Timer-forced preemption, end to end | 1.51 µs | 3.93 µs | 7.54 µs | 26.8 µs |
|   … of which Shale's path (timer handler entry → next thread running) | **388 ns** | 801 ns | 863 ns | 12.1 µs |
|   … of which interrupt delivery by the hypervisor/emulator | 1.12 µs | 3.06 µs | 6.60 µs | — |
| Process → process via `sys_yield` (syscall + scheduler + CR3 + FXSAVE/FXRSTOR + sysret) | **260 ns** | 290 ns | 20.3 µs | 34.5 µs |
| `getpid` round trip from ring 3 | 65 ns | 79 ns | 16.2 µs | 37.9 µs |
| Back-to-back `rdtsc` (measurement floor) | 18 ns | 24 ns | 17 ns | 19 ns |

Notes:
- **Preemption** samples the gap between the last instruction the preempted
  thread executed and the first one the next thread executes. Each sample is
  split at the timer handler's entry timestamp, which separates Shale's code
  from the hypervisor's interrupt injection.
- **Process-switch** runs varied between 196 and 263 ns median across boots
  on the shared host.
- **TCG ring transitions** cost about 16 µs in QEMU 8.2's emulator, whatever
  the return path (`sysret=0` forces `iretq`, same cost) and whatever
  PGE/SMAP/SMEP/UMIP are set to (`nopge nosmap nosmep noumip`). It isn't
  retranslation either: `-d in_asm` shows `syscall_entry` translated once.
  Kernel-internal switches under TCG take 165 ns, so the cost is the
  emulator's privilege transition, not Shale's code.

## Memory allocators

`host/tools/fragbench`, 5 seeds. It runs the same `lib/mm` code the kernel
runs, on traces replayed identically to every allocator.

### Page allocators

1 GiB, power-of-two requests (1–512 pages), mixed short and long lifetimes,
3M requests measured after a 600k-request warm-up.

| Allocator | Occupancy | Free memory in aligned 64 KiB blocks | Free memory in aligned 2 MiB blocks | 2 MiB requests failed despite enough free memory |
|---|---|---|---|---|
| **Buddy (Shale)** | 79.2% | **98.9%** | **89.2%** | 0 |
| First-fit extent list | 79.2% | 95.5% | 77.6% | 0 |
| Next-fit bitmap | 41.6% | 28.2% | 0.0% | 28,331 |

Of free memory, 10.8% is unusable for a 2 MiB allocation with the buddy
allocator, against 22.4% with first-fit. That is 52% less fragmentation. The
next-fit bitmap, a common hobby-kernel design, scatters single pages until
no 2 MiB block survives.

### Kernel heap

A kernel-object trace (inodes, dentries, tasks, VM areas, buffers, names,
packets), about 42k live objects and 15 MB live. 4M allocations measured
after 1M of warm-up.

| Heap | Steady-state overhead (1 − live/footprint) | Peak footprint | Footprint after freeing 90% of objects |
|---|---|---|---|
| **Slab, typed caches (Shale)** | **5.2%** | 16.1 MiB | **11.9 MiB** |
| Slab, size classes only | 18.7% | 18.7 MiB | 9.7 MiB |
| First-fit, boundary tags | 9.4% | 17.0 MiB | 16.3 MiB |
| Best-fit, boundary tags | 6.3% | 16.2 MiB | 15.9 MiB |

Typed caches cut wasted memory by 44% against first-fit and 17% against
best-fit. Size-class rounding alone would make the slab the worst of the
four (18.7%). That is why fixed-size kernel objects get dedicated caches,
like Linux's `kmem_cache_create`. After a 90% cache shrink, the slab heaps
return memory the boundary-tag heaps can't (11.9 vs 16.3 MiB).

## File system performance

`run=/bin/fsbench` under KVM, 5 boots. The disk is the host's page cache, and
every flush is a host `fdatasync`.

| Measurement | Median |
|---|---|
| Create + write 4 KiB + `fsync`, per file (p50 / p99) | 0.96 ms / 2.1 ms |
| 256 × 512-byte appends with `fsync` after each, total | 172 ms |
| The same 256 appends, then one `fsync` (group commit) | 1.07 ms (161× faster) |
| Sequential write, 16 MiB in 64 KiB writes + `fsync` | 1,518 MiB/s |
| Sequential read, 16 MiB, buffer cache cold (8 MiB cache) | 168 MiB/s |
| Create empty file / unlink (no `fsync`) | 195k / 800k ops/s |

Every commit costs two flushes: ordered data, then the journal. Reads are the
weak spot. Read-ahead batches up to 64 requests behind one doorbell, but each
4 KiB block is still its own virtio request through QEMU's legacy virtio-blk.
Merging contiguous blocks into one request is the next step.

## Robustness

| Check | Result |
|---|---|
| In-kernel tests (`make ktest`) | 34/34 under KVM and TCG; also on CI (AMD EPYC, KVM and TCG) |
| Host tests (`make htest`) | 21/21 (allocators against shadow models, ELF rules, ShaleFS model checks) |
| Syscall fuzzing from ring 3 (`sysfuzz`, 3 seeds × 200k calls) | 0 kernel crashes; free memory back to the byte afterwards; allocator invariants hold |
| ELF validator under libFuzzer + ASan/UBSan | 27.7M executions in 2 min, no crash or postcondition violation (CI: 19.4M in 1 min) |
| Faults in ring 3 (16 kinds: null, kernel read/write, W^X, NX stack/heap, `cli`/`hlt`/port I/O/CR3, UMIP `sgdt`, stack overflow, `#UD`, `#DE`, hostile pointers to syscalls) | Only the faulting process dies, with the right status |
| 100 spawn/exit cycles | Free physical memory identical before and after |

## Bugs the testing found

- **AMD `SYSRET` (found by CI).** AMD CPUs take the RPL bits of
  `STAR[63:48]` as written, while Intel forces RPL 3. User code ran with RPL-0
  selectors, and the next interrupt's `iretq` raised #GP(0x20). Fixed by
  storing `0x18 | 3`, as Linux does.
- **Ordered-mode gap (found while writing the crash oracle).** Data that
  eviction wrote back early wasn't flushed before the next journal write.
  Commits now flush whenever the cache has unflushed writes.
- **Fault printer.** A fault report used an unsupported format flag, faulted
  while formatting, and hid the original fault.
