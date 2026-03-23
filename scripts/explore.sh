#!/bin/bash
set -euo pipefail

TRACE_FILE="${1:-tests/cm2-im-optklib2.nptr.zst}"
OUT_ROOT="${2:-exploration}"
EXE="./build/npsim.elf"

run_case() {
  local tag="$1"
  shift
  echo "[$tag]"
  "$EXE" "$TRACE_FILE" \
    --freq-mhz 1000 \
    --l1i-pref none \
    --l1d-pref none \
    --outdir "$OUT_ROOT/$tag" \
    --print-none \
    "$@"
}

echo "Running exploration into simout/$OUT_ROOT"

run_case baseline \
  --l1i-size 1024 --l1i-blksize 16 --l1i-assoc 1 \
  --l1d-size 512 --l1d-blksize 16 --l1d-assoc 1 \
  --bpu-type bimodal --bpu-size 256 --btb-size 128 --ras-size 8

run_case icache_512 \
  --l1i-size 512 --l1i-blksize 16 --l1i-assoc 1 \
  --l1d-size 512 --l1d-blksize 16 --l1d-assoc 1 \
  --bpu-type bimodal --bpu-size 256 --btb-size 128 --ras-size 8

run_case dcache_none \
  --l1i-size 1024 --l1i-blksize 16 --l1i-assoc 1 \
  --l1d-size 0 \
  --bpu-type bimodal --bpu-size 256 --btb-size 128 --ras-size 8

run_case blk_32 \
  --l1i-size 1024 --l1i-blksize 32 --l1i-assoc 1 \
  --l1d-size 512 --l1d-blksize 32 --l1d-assoc 1 \
  --bpu-type bimodal --bpu-size 256 --btb-size 128 --ras-size 8

run_case bpu_bimodal_64 \
  --l1i-size 1024 --l1i-blksize 16 --l1i-assoc 1 \
  --l1d-size 512 --l1d-blksize 16 --l1d-assoc 1 \
  --bpu-type bimodal --bpu-size 64 --btb-size 64 --ras-size 8

run_case bpu_btfnt \
  --l1i-size 1024 --l1i-blksize 16 --l1i-assoc 1 \
  --l1d-size 512 --l1d-blksize 16 --l1d-assoc 1 \
  --bpu-type btfnt

run_case bpu_alwaystaken \
  --l1i-size 1024 --l1i-blksize 16 --l1i-assoc 1 \
  --l1d-size 512 --l1d-blksize 16 --l1d-assoc 1 \
  --bpu-type alwaystaken

OUT_ROOT="$OUT_ROOT" python3 - <<'PY'
import json
import os
from pathlib import Path

root = Path("simout") / os.environ["OUT_ROOT"]
print(f"\n{'Config':<18} {'IPC':>10} {'iMiss':>10} {'dMiss':>10}")
for stats_path in sorted(root.glob("*/stats.json")):
    data = json.load(stats_path.open())
    stats = data["stats0"]
    core = stats["Core"]["ipc"]
    imiss = stats["iCache"]["miss_rate"]
    dmiss = stats["dCache"]["miss_rate"] if "dCache" in stats else 0.0
    print(f"{stats_path.parent.name:<18} {core:>10.4f} {imiss:>10.4f} {dmiss:>10.4f}")
PY
