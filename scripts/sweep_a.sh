#!/bin/bash
# Sweep (a): Bimodal BP, varying iCache config
# Cache sizes {256B, 512B, 1K, 4K}, line sizes {16B, 32B, 64B}, assoc {1, 2}
set -euo pipefail

NPSIM="${NPSIM_HOME:-$(dirname "$0")/..}/build/npsim.elf"
OUTDIR="${NPSIM_HOME:-$(dirname "$0")/..}/simout/sweep_a"
TRACES=(
  "coremark-10rnd-vld"
  "micro-train-vld"
)
TRACEDIR="${NPSIM_HOME:-$(dirname "$0")/..}/tests"

mkdir -p "$OUTDIR"

SIZES=( 256 512 1024 4096 )
LINES=( 16 32 64 )
ASSOCS=( 1 2 )
JOBS=0
MAX_JOBS=4

for trace in "${TRACES[@]}"; do
  for sz in "${SIZES[@]}"; do
    for ln in "${LINES[@]}"; do
      for assoc in "${ASSOCS[@]}"; do
        tag="${trace}_sz${sz}_ln${ln}_a${assoc}"
        $NPSIM "$TRACEDIR/${trace}.nptr.zst" \
          --l1i-size "${sz}B" --l1i-blksize "$ln" --l1i-assoc "$assoc" \
          --bpu-type bimodal --bpu-size 16 --btb-size 16 \
          --br-pen 9 --print-none \
          -O "$OUTDIR/${tag}.json" &
        JOBS=$((JOBS + 1))
        if [ "$JOBS" -ge "$MAX_JOBS" ]; then
          wait -n
          JOBS=$((JOBS - 1))
        fi
      done
    done
  done
done

wait
echo "Sweep (a) complete. Results in $OUTDIR/"
