#!/usr/bin/env python3
"""Boot Shale under QEMU + OVMF and report how the run ended.

The kernel reads its command line from fw_cfg (opt/shale/cmdline) and ends a
test or benchmark run through the isa-debug-exit device:
    exit status 33 = pass, 35 = failure, 37 = panic.

Examples:
    tools/qemu.py --cmdline "test=all"
    tools/qemu.py --cmdline "bench=ctxswitch" --accel tcg --log build/bench.log
    tools/qemu.py --interactive --data-disk
"""
import argparse
import os
import selectors
import shutil
import subprocess
import sys
import tempfile
import time

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
EXIT_PASS, EXIT_FAIL, EXIT_PANIC = 33, 35, 37


def kvm_usable():
    return os.access("/dev/kvm", os.R_OK | os.W_OK)


def build_command(args, vars_path):
    accel = args.accel
    if accel == "auto":
        accel = "kvm" if kvm_usable() else "tcg"
    cpu = args.cpu or ("host" if accel == "kvm" else "max")
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-accel", accel, "-cpu", cpu,
        "-m", args.mem, "-smp", "1", "-nodefaults", "-no-reboot", "-display", "none",
        "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
        "-drive", f"if=pflash,format=raw,file={vars_path}",
        "-drive", f"if=ide,format=raw,media=disk,snapshot=on,file={args.esp}",
        "-serial", "stdio",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-fw_cfg", "name=opt/shale/cmdline,string=" + args.cmdline.replace(",", ",,"),
    ]
    for i, disk in enumerate(args.disk):
        cmd += ["-drive", f"if=none,id=data{i},format=raw,cache=writeback,file={disk}",
                "-device", f"virtio-blk-pci,drive=data{i},disable-modern=on"]
    if args.gdb:
        cmd += ["-s", "-S"]
    cmd += args.qemu_arg
    return accel, cmd


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--esp", default="build/esp.img")
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--accel", choices=["auto", "kvm", "tcg"], default="auto")
    ap.add_argument("--cpu", help="QEMU -cpu model (default: host under KVM, max under TCG)")
    ap.add_argument("--mem", default="512M")
    ap.add_argument("--disk", action="append", default=[], help="raw image attached as virtio-blk (repeatable)")
    ap.add_argument("--data-disk", action="store_true", help="attach a fresh 64 MiB scratch disk")
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--log", help="also write serial output to this file")
    ap.add_argument("--quiet", action="store_true", help="don't echo serial output")
    ap.add_argument("--interactive", action="store_true", help="attach the serial console to this terminal")
    ap.add_argument("--gdb", action="store_true", help="wait for gdb on :1234")
    ap.add_argument("--qemu-arg", action="append", default=[], help="extra QEMU argument (repeatable)")
    args = ap.parse_args()

    if not os.path.exists(args.esp):
        sys.exit(f"qemu.py: {args.esp} missing; run make first")
    tmp = tempfile.mkdtemp(prefix="shale-qemu-")
    try:
        vars_path = os.path.join(tmp, "OVMF_VARS.fd")
        shutil.copy(OVMF_VARS, vars_path)
        if args.data_disk:
            path = os.path.join(tmp, "data.img")
            with open(path, "wb") as f:
                f.truncate(64 << 20)
            args.disk.insert(0, path)
        accel, cmd = build_command(args, vars_path)
        if args.interactive:
            return subprocess.call(cmd)
        return run_captured(args, accel, cmd)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run_captured(args, accel, cmd):
    log = open(args.log, "w") if args.log else None
    start = time.monotonic()
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    timed_out = False
    pending = b""
    while True:
        remaining = args.timeout - (time.monotonic() - start)
        if remaining <= 0:
            timed_out = True
            proc.kill()
            break
        if not sel.select(timeout=min(remaining, 1.0)):
            if proc.poll() is not None:
                break
            continue
        chunk = os.read(proc.stdout.fileno(), 65536)
        if not chunk:
            break
        pending += chunk
        *lines, pending = pending.split(b"\n")
        for raw in lines:
            line = raw.decode("utf-8", "replace").rstrip("\r")
            if log:
                log.write(line + "\n")
            if not args.quiet:
                print(line, flush=True)
    if pending:
        line = pending.decode("utf-8", "replace")
        if log:
            log.write(line + "\n")
        if not args.quiet:
            print(line, flush=True)
    code = proc.wait()
    elapsed = time.monotonic() - start
    if log:
        log.close()
    if timed_out:
        print(f"qemu.py: TIMEOUT after {args.timeout:.0f}s ({accel})", file=sys.stderr)
        return 124
    verdict = {EXIT_PASS: "PASS", EXIT_FAIL: "FAIL", EXIT_PANIC: "PANIC"}.get(code, f"exit {code}")
    print(f"qemu.py: {verdict} in {elapsed:.1f}s ({accel})", file=sys.stderr)
    return 0 if code == EXIT_PASS else 1


if __name__ == "__main__":
    sys.exit(main())
