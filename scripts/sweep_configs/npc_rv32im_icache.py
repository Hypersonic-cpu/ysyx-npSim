# NPC-mode RV32IM calibration: iCache size x block size
# No BPU, default dCache (1024B/16B/1-way), NPC mode @ 500 MHz.
# Run with:
#   python3 scripts/sweep_2d.py --conf scripts/sweep_configs/npc_rv32im_icache.py \
#       --prefix coremark --outdir npc-rv32im-icache --no-area --jobs 4

trace = "tests/coremark-rv32im-npc.nptr.zst"

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
    "br-pen":         "1",
    "sdram-lat-us":   "0.043",
    "sdram-burst-us": "0.016",
    "ifq-size":       "4",
    "freq-mhz":       "500",
    "npc-mode":       True,
}
