#!/bin/bash
TRACE_FILE="tests/coremark.nptr.xz"
OUT_DIR="simout/exploration"
mkdir -p $OUT_DIR
EXE="./build/npsim.elf"

echo "Running Design Space Exploration..."

# 1. Baseline: 1KB iCache, 512B dCache, Bimodal(4K)
echo "Baseline..."
$EXE $TRACE_FILE --l1i-size 1024 --l1d-size 512 --outfile $OUT_DIR/baseline.json > /dev/null

# 2. iCache Size Exploration: 512B vs 1KB
echo "iCache Size..."
$EXE $TRACE_FILE --l1i-size 512 --l1d-size 512 --outfile $OUT_DIR/icache_512.json > /dev/null

# 3. dCache Size Exploration: None (0) vs 512B
echo "dCache Size..."
$EXE $TRACE_FILE --l1i-size 1024 --l1d-size 0 --outfile $OUT_DIR/dcache_none.json > /dev/null

# 4. Cache Block Size: 16B vs 32B (Default 16)
echo "Block Size..."
$EXE $TRACE_FILE --l1i-size 1024 --l1d-size 512 --l1i-blksize 32 --l1d-blksize 32 --outfile $OUT_DIR/blk_32.json > /dev/null

# 5. BTB/Predictor Exploration
# a. Bimodal 2-bit + 16 entries (2^4)
echo "Bimodal 16..."
$EXE $TRACE_FILE --bpu-type bimodal --bpu-entries 4 --outfile $OUT_DIR/bpu_bimodal_16.json > /dev/null

# b. Bimodal 2-bit + 4 entries (2^2)
echo "Bimodal 4..."
$EXE $TRACE_FILE --bpu-type bimodal --bpu-entries 2 --outfile $OUT_DIR/bpu_bimodal_4.json > /dev/null

# c. BTFNT (Static)
echo "BTFNT..."
$EXE $TRACE_FILE --bpu-type btfnt --outfile $OUT_DIR/bpu_btfnt.json > /dev/null

# d. Always Taken
echo "Always Taken..."
$EXE $TRACE_FILE --bpu-type alwaystaken --outfile $OUT_DIR/bpu_always.json > /dev/null

# e. RAS
echo "RAS..."
$EXE $TRACE_FILE --use-ras --outfile $OUT_DIR/bpu_ras.json > /dev/null

# 6. IF Queue Size Exploration: 2 vs 3 vs 4
echo "IF Queue Size..."
$EXE $TRACE_FILE --ifq-size 2 --outfile $OUT_DIR/ifq_2.json > /dev/null
$EXE $TRACE_FILE --ifq-size 4 --outfile $OUT_DIR/ifq_4.json > /dev/null

echo "Exploration Complete. Results in $OUT_DIR"

# Extract and Print Summary
echo "Summary:"
echo "Config,IPC,iMissRate,dMissRate"
for f in $OUT_DIR/*.json; do
    name=$(basename $f .json)
    ipc=$(grep "ipc" $f | head -1 | awk -F': ' '{print $2}' | sed 's/,//')
    imiss=$(grep "miss_rate" $f | head -1 | awk -F': ' '{print $2}' | sed 's/,//') # First miss_rate is iCache (usually)
    # Actually need better parsing, JSON order is config, stats.
    # We can use jq if available, or just greedy grep.
    # The first 'miss_rate' in stats section is likely iCache if iCache is first in simlist.
    # In main.cc: pipe, icache, dcache, bpu.
    # So stats keys: Pipeline, iCache, dCache, BimodalPredictor.
    # Pipeline has no miss_rate.
    # iCache has miss_rate.
    
    # Python helper for summary
done

python3 -c "
import json
import os
import glob

print(f'{'Config':<20} {'IPC':<10} {'iMiss':<10} {'dMiss':<10}')
for f in sorted(glob.glob('$OUT_DIR/*.json')):
    name = os.path.basename(f).replace('.json', '')
    try:
        with open(f) as fd:
            data = json.load(fd)
            # Find last stats dump (e.g. stats0, stats1... or just largest)
            stats_keys = [k for k in data.keys() if k.startswith('stats')]
            if not stats_keys: continue
            last_stats = data[stats_keys[-1]]
            
            ipc = last_stats['Pipeline']['ipc']
            imiss = last_stats['iCache']['miss_rate']
            dmiss = 0.0
            if 'dCache' in last_stats:
                dmiss = last_stats['dCache']['miss_rate']
            
            print(f'{name:<20} {ipc:.4f}     {imiss:.4f}     {dmiss:.4f}')
    except Exception as e:
        print(f'{name}: Error {e}')
"
