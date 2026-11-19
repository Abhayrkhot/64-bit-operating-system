#!/usr/bin/env bash
# Runs a command inside the shaledev toolchain container with the repo at /src.
# /dev/kvm is passed through when present so QEMU can use KVM.
#
#   scripts/dev.sh 'make -j test'
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
image="${SHALE_DEV_IMAGE:-shaledev:24.04}"
if ! docker image inspect "$image" >/dev/null 2>&1; then
  docker build -t "$image" -f "$repo/docker/dev.Dockerfile" "$repo/docker"
fi
args=(--rm -u "$(id -u):$(id -g)" -e HOME=/tmp/home -v "$repo:/src" -w /src)
[ -t 0 ] && args+=(-it)
if [ -e /dev/kvm ]; then
  args+=(--device /dev/kvm --group-add "$(stat -c %g /dev/kvm)")
fi
exec docker run "${args[@]}" "$image" bash -c "mkdir -p /tmp/home; $*"
