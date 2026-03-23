# NPC-mode calibration sweep: iCache size × line size
# No BPU, no dCache, matching RTL NPC-mode parameters.
# Run with:
#   python3 scripts/sweep_2d.py --conf scripts/sweep_configs/npc_cal.py \
#       --prefix coremark --outdir npc-cal --fig-type heatmap
#
# RTL reference: $NPC_HOME/ccout/sweep-cache-coremark/
#   Error heatmap: python3 visual/plot_error_heatmap.py \
#       --sim-dir simout/npc-cal \
#       --rtl-dir $NPC_HOME/ccout/sweep-cache-coremark \
#       --prefix coremark --npc-mode \
#       --icache scripts/sweep_configs/npc_cal.py \
#       --outfile visual/plots/npc-cal/error_heatmap.png

trace = "tests/coremark-npc-cal2.nptr.zst"

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
    "l1i-assoc":      "1",
    "l1d-size":       "0",
    "bpu-type":       "none",
    "stbuf-entries":  "0",
}
