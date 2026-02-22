#!/bin/bash
# Sweep (b): 512B/16B/1-way iCache, sweep BPs and iCache prefetchers
set -euo pipefail

NPSIM="${NPSIM_HOME:-$(dirname "$0")/..}/build/npsim.elf"
OUTDIR="${NPSIM_HOME:-$(dirname "$0")/..}/simout/sweep_b"
TRACES=(
  "coremark-10rnd-vld"
  "micro-train-vld"
)
TRACEDIR="${NPSIM_HOME:-$(dirname "$0")/..}/tests"

mkdir -p "$OUTDIR"

BPUS=( "none" "bimodal" "gshare" "tournament" "alwaystaken" "btfnt" )
IPFS=( "none" "nextline" "stride" "tagged" )
JOBS=0
MAX_JOBS=4

CACHE_ARGS="--l1i-size 512B --l1i-blksize 16 --l1i-assoc 1"

for trace in "${TRACES[@]}"; do
  for bpu in "${BPUS[@]}"; do
    for ipf in "${IPFS[@]}"; do
      tag="${trace}_bp-${bpu}_pf-${ipf}"
      ipf_arg=""
      [ "$ipf" != "none" ] && ipf_arg="--ipf $ipf"
      bpu_args="--bpu-type $bpu"
      [ "$bpu" != "none" ] && bpu_args="$bpu_args --bpu-size 16 --btb-size 16"

      $NPSIM "$TRACEDIR/${trace}.nptr.zst" \
        $CACHE_ARGS $bpu_args $ipf_arg \
        --br-pen 9 --print-none \
        --outdir "sweep_b/${tag}" &
      JOBS=$((JOBS + 1))
      if [ "$JOBS" -ge "$MAX_JOBS" ]; then
        wait -n
        JOBS=$((JOBS - 1))
      fi
    done
  done
done

wait
echo "Sweep (b) complete. Results in $OUTDIR/"
