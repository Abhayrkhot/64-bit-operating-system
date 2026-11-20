#!/usr/bin/env python3
"""End-to-end crash test: kill QEMU while the kernel runs fsstress on ShaleFS.

Each trial:
  1. format a fresh disk image on the host (build/host/shalefs mkfs)
  2. boot Shale with run=/bin/fsstress; SIGKILL QEMU at a random moment
     after the workload starts (a power cut as far as the guest is concerned)
  3. boot Shale again in `fsck` mode: the kernel mounts the disk (replaying
     its journal), unmounts and runs fsck
  4. on the host, run fsck again and check fsstress's durability log against
     the recovered files:
       - DURABLE files not MODIFYed afterwards: exact length and CRC32C
       - REPLACEd files: one complete version, none older than the last
         replace whose fsync returned

    tools/crashtest.py --trials 100 --ops 4000
"""
import argparse
import os
import random
import re
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHALEFS = os.path.join(ROOT, "build/host/shalefs")
QEMU_PY = os.path.join(ROOT, "tools/qemu.py")


def qemu_cmd(cmdline, disk, accel):
    return [sys.executable, QEMU_PY, "--cmdline", cmdline, "--disk", disk, "--accel", accel, "--timeout", "120"]


def run_until_killed(disk, seed, ops, kill_after, accel):
    """Boots fsstress and kills QEMU kill_after seconds after FSSTRESS START.
    Returns the console lines seen and whether the workload finished first."""
    proc = subprocess.Popen(qemu_cmd(f"run=/bin/fsstress args={seed}:{ops}", disk, accel),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
    lines, started_at, finished = [], None, False
    os.set_blocking(proc.stdout.fileno(), False)
    pending = b""
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        try:
            chunk = proc.stdout.read()
        except BlockingIOError:
            chunk = None
        if chunk:
            pending += chunk
            *done, pending = pending.split(b"\n")
            for raw in done:
                line = raw.decode("utf-8", "replace").rstrip("\r")
                lines.append(line)
                if line.startswith("FSSTRESS START"):
                    started_at = time.monotonic()
                if line.startswith("FSSTRESS DONE"):
                    finished = True
        if started_at is not None and time.monotonic() - started_at >= kill_after:
            break
        if proc.poll() is not None:
            break
        time.sleep(0.002)
    # Kill the whole process group (qemu.py and QEMU itself), uncleanly.
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    proc.wait()
    return lines, finished


def recover_in_kernel(disk, accel):
    out = subprocess.run(qemu_cmd("fsck", disk, accel), capture_output=True, text=True, errors="replace")
    m = re.search(r"FSCK errors=(\d+) .*replayed_txns=(\d+)", out.stdout)
    if not m:
        return None, None, out.stdout[-2000:]
    return int(m.group(1)), int(m.group(2)), out.stdout


def host_crc(disk, path):
    out = subprocess.run([SHALEFS, "crc", disk, path], capture_output=True, text=True).stdout.strip()
    if out == "missing" or not out:
        return None
    n, crc = out.split()
    return int(n), int(crc)


def check_durability(disk, lines):
    """Returns a list of violations of the durability log."""
    durable, modified_after = {}, {}
    replace = {}  # target -> {"floor": index or None, "versions": [(len, crc)]}
    for line in lines:
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "DURABLE" and len(parts) == 4:
            durable[parts[1]] = (int(parts[2]), int(parts[3]))
            modified_after[parts[1]] = False
        elif parts[0] == "MODIFY" and len(parts) == 2:
            if parts[1] in durable:
                modified_after[parts[1]] = True
        elif parts[0] == "REPLACE-BEGIN" and len(parts) == 4:
            r = replace.setdefault(parts[1], {"floor": None, "versions": []})
            r["versions"].append((int(parts[2]), int(parts[3])))
        elif parts[0] == "REPLACE" and len(parts) == 4:
            r = replace.setdefault(parts[1], {"floor": None, "versions": []})
            v = (int(parts[2]), int(parts[3]))
            r["floor"] = max(i for i, x in enumerate(r["versions"]) if x == v)
    problems = []
    for path, want in durable.items():
        if modified_after[path]:
            continue
        got = host_crc(disk, path)
        if got != want:
            problems.append(f"{path}: expected {want}, found {got}")
    for target, r in replace.items():
        got = host_crc(disk, target)
        allowed = r["versions"][r["floor"]:] if r["floor"] is not None else r["versions"]
        if got is None:
            if r["floor"] is not None:
                problems.append(f"{target}: vanished after a durable replace")
        elif got not in allowed:
            problems.append(f"{target}: {got} is not a complete allowed version")
    return problems, len(durable), len(replace)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--trials", type=int, default=20)
    ap.add_argument("--ops", type=int, default=4000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--max-kill", type=float, default=3.0, help="kill up to this many seconds into the workload")
    ap.add_argument("--accel", default="auto")
    ap.add_argument("--disk-mib", type=int, default=64)
    ap.add_argument("--keep-failures", default="build/crashtest-failures")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    totals = {"trials": 0, "killed_mid_run": 0, "fsck_errors": 0, "durability": 0, "replayed": 0, "checked_files": 0}
    tmp = tempfile.mkdtemp(prefix="shale-crash-")
    for trial in range(args.trials):
        disk = os.path.join(tmp, "disk.img")
        subprocess.run([SHALEFS, "mkfs", disk, str(args.disk_mib)], check=True, capture_output=True)
        seed = rng.randrange(1, 1 << 30)
        kill_after = rng.uniform(0.05, args.max_kill)
        lines, finished = run_until_killed(disk, seed, args.ops, kill_after, args.accel)
        errors, replayed, log = recover_in_kernel(disk, args.accel)
        host = subprocess.run([SHALEFS, "fsck", disk], capture_output=True, text=True)
        problems, ndurable, nreplace = check_durability(disk, lines)
        ok = errors == 0 and host.returncode == 0 and not problems
        totals["trials"] += 1
        totals["killed_mid_run"] += 0 if finished else 1
        totals["fsck_errors"] += 0 if (errors == 0 and host.returncode == 0) else 1
        totals["durability"] += 1 if problems else 0
        totals["replayed"] += replayed or 0
        totals["checked_files"] += ndurable + nreplace
        status = "ok" if ok else "FAIL"
        print(f"trial {trial:3d} seed {seed:10d} kill {kill_after:4.2f}s {'(finished)' if finished else '':10s} "
              f"replayed {replayed} txns, {ndurable} durable files, {nreplace} replaced: {status}", flush=True)
        if not ok:
            os.makedirs(args.keep_failures, exist_ok=True)
            keep = os.path.join(args.keep_failures, f"trial{trial}-seed{seed}.img")
            os.replace(disk, keep)
            print(f"  kernel fsck: {errors}; host: {host.stdout.strip()}")
            for p in problems[:5]:
                print(f"  {p}")
            if errors is None:
                print(log)
    print("CRASHTEST " + " ".join(f"{k}={v}" for k, v in totals.items()))
    bad = totals["fsck_errors"] + totals["durability"]
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
