#!/bin/bash
set -euo pipefail

DRYRUN=0

OUTROOT="sweep"
BENCH_NAME=micro-train
BENCH_PATH="$NPSIM_HOME/tests/$BENCH_NAME.nptr.zst"
NPSIM="$NPSIM_HOME/build/npsim.elf"
NPSIM_ARGS=" --print-none "

if [[ $# -ge 1 ]] && [[ "$1" == "--dry-run" || "$1" == "--dryrun" ]]; then
  DRYRUN=1
  shift
  NPSIM="echo $NPSIM"
fi

# Existence check
mkdir -p $OUTROOT
if [[ ! -f "$BENCH_PATH" ]]; then
  echo "Trace $BENCH_PATH does not exist";
  exit 1;
fi

# Build
if [[ "$DRYRUN" -ne 1 ]]; then
  make clean && make EXEMODE=1 all
fi

L1D_SIZES=( "0B" )
L1I_SIZES=( "256B" "512B" "1kB" )
L1I_ASSOC=( "1" "2" "4" )
L1I_BLKSZ=( "8" "16" "32" )

BPU_TYPES=( "none" "bimodal" )
BPU_SIZES=( "8" "16" "32" "64" )
BTB_SIZES=( "8" "16" "32" "64" )

for dsz in "${L1D_SIZES[@]}"; do
  for isz in "${L1I_SIZES[@]}"; do
    for iassoc in "${L1I_ASSOC[@]}"; do
      for iblksz in "${L1I_BLKSZ[@]}"; do
        cache_cmd="--l1d-size $dsz --l1i-size $isz --l1i-assoc $iassoc --l1i-blksize $iblksz"
        out_prefix="-l1i${isz}_a${iassoc}_b${iblksz}-l1d${dsz}"
        for tp in "${BPU_TYPES[@]}"; do
          if [[ "$tp" == "none" ]]; then
            bp_cmd="--bpu-type $tp"
            outname="${OUTROOT}/${BENCH_NAME}${out_prefix}-nobp"
            $NPSIM $BENCH_PATH $NPSIM_ARGS $cache_cmd $bp_cmd --outfile ${outname}.json &
          else
            for bpusz in "${BPU_SIZES[@]}"; do
              for btbsz in "${BTB_SIZES[@]}"; do
                bp_cmd="--bpu-type $tp --bpu-size $bpusz --btb-size $btbsz"
                outname="${OUTROOT}/${BENCH_NAME}${out_prefix}-${tp}bp_sz${bpusz}_btb${btbsz}"
                $NPSIM $BENCH_PATH $NPSIM_ARGS $cache_cmd $bp_cmd --outfile ${outname}.json &
              done
              wait
              echo "> Group ${out_prefix}"
            done
          fi
        done
      done
    done
  done
done

wait
echo "== All Simulations Done =="
