# cfg_ext_valid.py — Combined iCache × dCache × BPU validation sweep (SoC mode)
#
# 4×2 × 2 × 3 = 48 configs total.
# Uses coremark-soc-ext trace and SoC-calibrated timing params.
# Recalibrated Mar 2026: br-pen=7, axi-ovhd-cyc=4, ras-size=8.
#   48-config mean |err| = 1.39%, max |err| = 3.46%.
#
# Run sweep (1 GHz):
#   python3 scripts/sweep_2d.py \
#       --conf scripts/sweep_configs/cfg_ext_1ghz_mar06.py \
#       --prefix cm2_1000MHz_cfg-ext --outdir cm2_1000MHz_cfg-ext \
#       --no-area --jobs 8
#
# Plot (with --sweep-file shorthand):
#   python3 visual/plot_error_heatmap.py \
#       --sim-dir  simout/cm2_1000MHz_cfg-ext \
#       --rtl-dir  $NPC_HOME/ccout/cm2_1000MHz_cfg-ext \
#       --prefix   cm2_1000MHz_cfg-ext \
#       --sweep-file scripts/sweep_configs/cfg_ext_1ghz_mar06.py \
#       --outfile  visual/plots/mar06-cfg-ext/error_1ghz.png
#
# For each panel, non-active components are fixed at their first axis1 value:
#   iCache panel: dcache=512B-b16-a1, bpu=bimodal-h64-t64
#   dCache panel: icache=512B-b16-a1, bpu=bimodal-h64-t64
#   BPU panel:    icache=512B-b16-a1, dcache=512B-b16-a1

trace = "tests/coremark-soc-ext.nptr.zst"

# ── iCache: size × block size ─────────────────────────────────────────────────
icache_axis = {
    "axis1": {
        "name": "l1i_size",
        "vals": [
            {"l1i-size": "512B"},
            {"l1i-size": "1kB"},
            {"l1i-size": "2kB"},
            {"l1i-size": "4kB"},
        ],
        "labels": ["512B", "1kB", "2kB", "4kB"],
    },
    "axis2": {
        "name": "l1i_blksize",
        "vals": [
            {"l1i-blksize": "16"},
            {"l1i-blksize": "32"},
        ],
        "labels": ["16B", "32B"],
    },
    "fixed": {"l1i-assoc": "1"},
}

# ── dCache: size (block=16B fixed) ────────────────────────────────────────────
dcache_axis = {
    "axis1": {
        "name": "l1d_size",
        "vals": [
            {"l1d-size": "512B"},
            {"l1d-size": "1kB"},
        ],
        "labels": ["512B", "1kB"],
    },
    "fixed": {"l1d-blksize": "16", "l1d-assoc": "1"},
}

# ── BPU: bimodal with varying BTB size ────────────────────────────────────────
bpu_axis = {
    "axis1": {
        "name": "btb_size",
        "vals": [
            {"bpu-type": "bimodal", "bpu-size": "64",  "btb-size": "64"},
            {"bpu-type": "bimodal", "bpu-size": "128", "btb-size": "128"},
            {"bpu-type": "bimodal", "bpu-size": "256", "btb-size": "256"},
        ],
        "labels": ["btb64", "btb128", "btb256"],
    },
    "fixed": {"ras-size": "8"},
}

# ── Global timing params (SoC-mode, recalibrated Mar 2026) ─────────────
# stbuf-entries=0: StoreBuffer commented out in RTL (rvCore.scala)
# br-pen=7: misprediction penalty incl. pipeline drain + iCache
#           refill; calibrated across 48 configs (max err 3.5%)
# axi-ovhd-cyc=4: AXI protocol overhead per memory transaction
# sdram/sram: SoC-mode calibrated timing
default_conf = {
    "stbuf-entries":   "0",
    "br-pen":          "7",
    "axi-ovhd-cyc":    "4",
    "sdram-lat-us":    "0.051",
    "sdram-burst-us":  "0.024",
    "sram-lat":        "1",
    "ifq-size":        "3",
}
