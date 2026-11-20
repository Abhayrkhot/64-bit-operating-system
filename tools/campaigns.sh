#!/usr/bin/env bash
# Crash-consistency campaigns behind docs/RESULTS.md. Run inside the dev
# container after `make all`:
#
#   scripts/dev.sh 'tools/campaigns.sh'            # full (about 15 minutes on 20 cores)
#   SEEDS=3 STATES=2000 TRIALS=20 tools/campaigns.sh   # quick
set -uo pipefail
SEEDS=${SEEDS:-10}
STATES=${STATES:-10000}
OPS=${OPS:-1500}
TRIALS=${TRIALS:-200}
OUT=${OUT:-build/results}
mkdir -p "$OUT"

run_mode() {  # name, flags...
  local name=$1
  shift
  for s in $(seq 1 "$SEEDS"); do
    build/host/crashsim --seed "$s" --ops "$OPS" --states "$STATES" --quiet "$@" > "$OUT/crashsim-$name-seed$s.txt" &
  done
  wait
  echo "== crashsim $name"
  grep -h "^crashsim:" "$OUT"/crashsim-"$name"-seed*.txt
}

run_mode journal-torn --torn
run_mode nojournal
run_mode trust-commits-torn --trust-commits --torn

echo "== QEMU kill test ($TRIALS trials)"
python3 tools/crashtest.py --trials "$TRIALS" --ops 4000 --max-kill 2.0 | tee "$OUT/crashtest.txt" | tail -1
