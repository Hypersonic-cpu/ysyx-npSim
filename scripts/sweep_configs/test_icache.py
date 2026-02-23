# Test sweep: iCache size × line size, StBuf=2, no BPU
# Run with:
#   python3 scripts/sweep_2d.py --conf scripts/sweep_configs/test_icache.py \
#                               --outdir test_2d --fig-type both

trace = "tests/coremark-10rnd-vld.nptr.zst"

axis1 = {
    "name": "cache_size",
    "vals": [
        {"l1i-size": "256B"},
        {"l1i-size": "512B"},
        {"l1i-size": "1kB"},
        {"l1i-size": "4kB"},
    ],
    "labels": ["256B", "512B", "1kB", "4kB"],
}

axis2 = {
    "name": "line_size",
    "vals": [
        {"l1i-blksize": "8"},
        {"l1i-blksize": "16"},
        {"l1i-blksize": "32"},
        {"l1i-blksize": "64"},
    ],
    "labels": ["8B", "16B", "32B", "64B"],
}

default_conf = {
    "l1i-assoc":      "1",
    "bpu-type":       "none",
    "stbuf-entries":  "2",
    "br-pen":         "9",
}
