# SoC-mode RV32IM calibration: iCache size x block size
# Default dCache (1024B/16B/1-way), no BPU.
# Run twice for each frequency:
#   python3 scripts/sweep_2d.py --conf scripts/sweep_configs/soc_rv32im_icache.py \
#       --prefix coremark --outdir soc-rv32im-icache-500 --no-area --jobs 4
#   # Then edit freq-mhz to 1000 and --outdir soc-rv32im-icache-1000

trace = "tests/coremark-rv32im-soc.nptr.zst"

axis1 = {
    "name": "cache_size",
    "vals": [
        {"l1i-size": "512"},
        {"l1i-size": "1024"},
        {"l1i-size": "2048"},
        {"l1i-size": "4096"},
    ],
    "labels": ["512B", "1kB", "2kB", "4kB"],
}

axis2 = {
    "name": "line_size",
    "vals": [
        {"l1i-blksize": "16"},
        {"l1i-blksize": "32"},
    ],
    "labels": ["16B", "32B"],
}

default_conf = {
    "l1i-assoc":      "1",
    "l1d-size":       "1024",
    "l1d-blksize":    "16",
    "l1d-assoc":      "1",
    "bpu-type":       "none",
    "stbuf-entries":  "2",
    "sdram-lat-us":   "0.060",
    "sdram-burst-us": "0.020",
    "icache-sdram-extra-us": "0.039",
    "sram-lat":       "1",
    "freq-mhz":       "500",
}
