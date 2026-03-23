# Sweep (a) equivalent: iCache size vs line size
# Reproduces sweep_a (assoc=1) with coremark trace.
# Run with:
#   python3 sweep_2d.py --conf scripts/sweep_configs/cache_vs_line.py \
#                       --outdir 2d_cache_line --fig-type heatmap

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
    "l1i-assoc":  "1",
    "bpu-type":   "bimodal",
    "bpu-size":   "16",
    "btb-size":   "16",
}
