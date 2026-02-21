#!/bin/bash
# Sweep (c): dCache/StBuf sweep with ideal and real inst supply
# (c-1) ideal: 4kB cache, bimodal large, nextline pf
# (c-2) real:  512B cache, bimodal small, no pf
# Data side: StBuf{2,4,8}, dCache{256,512,1024,4096}×{stride,none}, line=16B
# Note: dCache PipeCache runs are slow; use micro-test for dCache configs
set -euo pipefail

NPSIM="${NPSIM_HOME:-$(dirname "$0")/..}/build/npsim.elf"
OUTDIR="${NPSIM_HOME:-$(dirname "$0")/..}/simout/sweep_c"
TRACEDIR="${NPSIM_HOME:-$(dirname "$0")/..}/tests"

mkdir -p "$OUTDIR"

# Ideal inst supply config
IDEAL_ICACHE="--l1i-size 4kB --l1i-blksize 16 --l1i-assoc 2 --bpu-type bimodal --bpu-size 64 --btb-size 64 --ipf nextline"
# Real inst supply config
REAL_ICACHE="--l1i-size 512B --l1i-blksize 16 --l1i-assoc 1 --bpu-type bimodal --bpu-size 4 --btb-size 4"

STBUF_SIZES=( 2 4 8 )
DCACHE_SIZES=( 256 512 1024 4096 )
DPFS=( "none" "stride" )
JOBS=0
MAX_JOBS=4

# Use all traces for StoreBuffer, only micro-test for dCache (perf)
STBUF_TRACES=( "coremark-10rnd-vld" "micro-test-vld" )
DCACHE_TRACES=( "micro-test-vld" )

for mode in ideal real; do
  if [ "$mode" = "ideal" ]; then
    ICACHE_ARGS=$IDEAL_ICACHE
  else
    ICACHE_ARGS=$REAL_ICACHE
  fi

  # StoreBuffer configs (fast — all traces)
  for trace in "${STBUF_TRACES[@]}"; do
    for stbsz in "${STBUF_SIZES[@]}"; do
      tag="${trace}_${mode}_stbuf${stbsz}"
      $NPSIM "$TRACEDIR/${trace}.nptr.zst" \
        $ICACHE_ARGS --stbuf-entries "$stbsz" \
        --br-pen 9 --print-none \
        -O "$OUTDIR/${tag}.json" &
      JOBS=$((JOBS + 1))
      if [ "$JOBS" -ge "$MAX_JOBS" ]; then
        wait -n
        JOBS=$((JOBS - 1))
      fi
    done
  done

  # dCache configs (slow — micro-test only)
  for trace in "${DCACHE_TRACES[@]}"; do
    for dsz in "${DCACHE_SIZES[@]}"; do
      for dpf in "${DPFS[@]}"; do
        dpf_arg=""
        [ "$dpf" != "none" ] && dpf_arg="--dpf $dpf"
        tag="${trace}_${mode}_dc${dsz}_pf-${dpf}"
        $NPSIM "$TRACEDIR/${trace}.nptr.zst" \
          $ICACHE_ARGS --l1d-size "${dsz}B" --l1d-blksize 16 --l1d-assoc 1 \
          $dpf_arg --br-pen 9 --print-none \
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
echo "Sweep (c) complete. Results in $OUTDIR/"
