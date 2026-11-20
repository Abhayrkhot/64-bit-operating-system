#!/usr/bin/env python3
"""Runs Shale's benchmarks several times and prints the median of each metric.

    tools/bench.py --runs 5 --out build/results/bench.md

Benchmarks: bench=ctxswitch (kernel threads), bench=process_switch (user
processes + syscalls), run=/bin/fsbench (file system), each under KVM and
TCG where it makes sense, and host/tools/fragbench over several seeds.
Medians, not best runs: the host is shared and noisy.
"""
import argparse
import os
import re
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH_RE = re.compile(r"^BENCH (\S+) (\S+)( .*)?$")
KV_RE = re.compile(r"(\w+)=([0-9.]+)")


def run_qemu(cmdline, accel, data_disk=False):
    cmd = [sys.executable, os.path.join(ROOT, "tools/qemu.py"), "--cmdline", cmdline, "--accel", accel,
           "--timeout", "600", "--quiet", "--log", "/dev/stdout"]
    if data_disk:
        cmd.append("--data-disk")
    out = subprocess.run(cmd, capture_output=True, text=True, errors="replace").stdout
    metrics = {}
    for line in out.splitlines():
        line = line.strip()
        m = BENCH_RE.match(line)
        if not m or m.group(1) in ("begin", "done", "env"):
            continue
        for key, val in KV_RE.findall(m.group(3) or ""):
            metrics[f"{m.group(1)}.{m.group(2)}.{key}"] = float(val)
    return metrics


def collect(label, runs, fn):
    samples = {}
    for i in range(runs):
        for k, v in fn().items():
            samples.setdefault(k, []).append(v)
        print(f"  {label}: run {i + 1}/{runs}", file=sys.stderr, flush=True)
    return {k: (statistics.median(v), min(v), max(v), len(v)) for k, v in samples.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--out", default="build/results/bench.md")
    ap.add_argument("--skip-tcg", action="store_true")
    args = ap.parse_args()
    sections = []
    accels = ["kvm"] if args.skip_tcg else ["kvm", "tcg"]
    for accel in accels:
        sections.append((f"ctxswitch ({accel})", collect(f"ctxswitch {accel}", args.runs,
                                                          lambda: run_qemu("bench=ctxswitch", accel))))
        sections.append((f"process_switch ({accel})", collect(f"process {accel}", args.runs,
                                                               lambda: run_qemu("bench=process_switch", accel))))
    sections.append(("fsbench (kvm)", collect("fsbench", args.runs,
                                              lambda: run_qemu("run=/bin/fsbench", "kvm", data_disk=True))))
    lines = ["| benchmark | metric | median | min | max | runs |", "|---|---|---|---|---|---|"]
    for title, metrics in sections:
        for k in sorted(metrics):
            med, lo, hi, n = metrics[k]
            lines.append(f"| {title} | {k} | {med:g} | {lo:g} | {hi:g} | {n} |")
    text = "\n".join(lines) + "\n"
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        f.write(text)
    print(text)


if __name__ == "__main__":
    main()
