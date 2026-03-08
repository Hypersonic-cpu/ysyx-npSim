# SoC-mode RV32IM calibration: dCache size sweep
# Default iCache (1024B/16B/1-way), no BPU.
# Run twice for each frequency:
#   python3 scripts/sweep_2d.py --conf scripts/sweep_configs/soc_rv32im_dcache.py \
#       --prefix coremark --outdir soc-rv32im-dcache-500 --no-area --jobs 4
#   # Then edit freq-mhz to 1000 and --outdir soc-rv32im-dcache-1000

trace = "tests/coremark-rv32im-soc.nptr.zst"

axis1 = {
    "name": "dcache_size",
    "vals": [
        {"l1d-size": "256"},
        {"l1d-size": "512"},
        {"l1d-size": "1024"},
        {"l1d-size": "2048"},
    ],
    "labels": ["256B", "512B", "1kB", "2kB"],
}

axis2 = {
    "name": "dcache_blk",
    "vals": [
        {"l1d-blksize": "16"},
    ],
    "labels": ["16B"],
}

default_conf = {
    "l1i-size":       "1024",
    "l1i-blksize":    "16",
    "l1i-assoc":      "1",
    "l1d-assoc":      "1",
    "bpu-type":       "none",
    "stbuf-entries":  "2",
    "sram-lat":       "1",
    "freq-mhz":       "500",
}
