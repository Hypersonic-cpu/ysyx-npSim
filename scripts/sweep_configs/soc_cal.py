# SoC-mode calibration sweep: iCache size × line size
# No BPU, no dCache, SoC address-based latency routing.
# Run with:
#   python3 scripts/sweep_2d.py --conf scripts/sweep_configs/soc_cal.py \
#                               --outdir soc-cal --fig-type heatmap

trace = "tests/coremark-soc-cal.nptr.zst"

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
    "l1i-assoc":       "1",
    "l1d-size":        "0",
    "bpu-type":        "none",
    "stbuf-entries":   "0",
    "br-pen":          "1",
    "mem-lat":         "70",
    "mem-bstlat":      "23",
    "ifq-size":        "3",
    "socmode":         None,
    "sram-lat":        "1",
    "sdram-single-lat":"52",
}
