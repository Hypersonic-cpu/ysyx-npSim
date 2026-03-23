# NPC-mode calibration sweep for MicroBench (train)
# Matches RTL configs in ccout/sweep-microbench-train/
# Run with:
#   python3 scripts/sweep_2d.py --conf scripts/sweep_configs/npc_cal_micro.py \
#       --prefix microbench --outdir npc-cal-micro --fig-type heatmap
#
# RTL reference: $NPC_HOME/ccout/sweep-microbench-train/
#   Error heatmap: python3 visual/plot_error_heatmap.py \
#       --sim-dir simout/npc-cal-micro \
#       --rtl-dir $NPC_HOME/ccout/sweep-microbench-train \
#       --prefix microbench --npc-mode \
#       --icache scripts/sweep_configs/npc_cal_micro.py \
#       --outfile visual/plots/npc-cal-micro/error_heatmap.png

trace = "tests/microbench-npc-cal2.nptr.zst"

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
    "l1i-assoc":      "1",
    "l1d-size":       "0",
    "bpu-type":       "none",
    "stbuf-entries":  "0",
}
