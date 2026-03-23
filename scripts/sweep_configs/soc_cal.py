# SoC-mode calibration sweep: iCache size × line size
# No BPU, no dCache, SoC address-based latency routing.
# Run with:
#   python3 scripts/sweep_2d.py --conf scripts/sweep_configs/soc_cal.py \
#       --prefix coremark --outdir soc-cal --fig-type heatmap
#
# RTL reference: $NPC_HOME/ccout/sweep-cache-coremark-soc/
#   Error heatmap: python3 visual/plot_error_heatmap.py \
#       --sim-dir simout/soc-cal \
#       --rtl-dir $NPC_HOME/ccout/sweep-cache-coremark-soc \
#       --prefix coremark \
#       --icache scripts/sweep_configs/soc_cal.py \
#       --outfile visual/plots/soc-cal/error_heatmap.png

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
    ],
    "labels": ["8B", "16B", "32B"],
}

default_conf = {
    "l1i-assoc":       "1",
    "l1d-size":        "0",
    "bpu-type":        "none",
    "stbuf-entries":   "0",
    "socmode":         None,
    "sram-lat":        "1",
}
