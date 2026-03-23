# SoC-mode calibration sweep for MicroBench (train)
# Matches RTL configs in ccout/sweep-microbench-soc-train/
# Run with:
#   python3 scripts/sweep_2d.py --conf scripts/sweep_configs/soc_cal_micro.py \
#       --prefix microbench --outdir soc-cal-micro --fig-type heatmap
#
# RTL reference: $NPC_HOME/ccout/sweep-microbench-soc-train/
#   Error heatmap: python3 visual/plot_error_heatmap.py \
#       --sim-dir simout/soc-cal-micro \
#       --rtl-dir $NPC_HOME/ccout/sweep-microbench-soc-train \
#       --prefix microbench \
#       --icache scripts/sweep_configs/soc_cal_micro.py \
#       --outfile visual/plots/soc-cal-micro/error_heatmap.png

trace = "tests/microbench-soc-cal.nptr.zst"

axis1 = {
    "name": "cache_size",
    "vals": [
        {"l1i-size": "512B"},
        {"l1i-size": "1kB"},
    ],
    "labels": ["512B", "1kB"],
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
    "l1i-assoc":       "1",
    "l1d-size":        "0",
    "bpu-type":        "none",
    "stbuf-entries":   "0",
    "socmode":         None,
    "sram-lat":        "1",
}
