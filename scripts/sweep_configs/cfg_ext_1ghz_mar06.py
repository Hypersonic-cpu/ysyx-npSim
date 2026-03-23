# cfg_ext_1ghz_mar06.py — iCache × dCache × BPU validation (SoC, 1 GHz)
#
# 4×2 × 2 × 3 = 48 configs.  Corrected calibration (stbuf=0 verified,
# StoreBuffer not used in RTL).
#
# Run:
#   python3 scripts/sweep_2d.py \
#       --conf scripts/sweep_configs/cfg_ext_1ghz_mar06.py \
#       --prefix cm2_1000MHz_cfg-ext --outdir cm2_1000MHz_cfg-ext \
#       --no-area --jobs 8
#
# Plot:
#   python3 visual/plot_error_heatmap.py \
#       --sim-dir  simout/cm2_1000MHz_cfg-ext \
#       --rtl-dir  $NPC_HOME/ccout/cm2_1000MHz_cfg-ext \
#       --prefix   cm2_1000MHz_cfg-ext \
#       --sweep-file scripts/sweep_configs/cfg_ext_1ghz_mar06.py \
#       --outfile  visual/plots/mar06-cfg-ext/error_1ghz.png

trace = "tests/coremark-soc-ext.nptr.zst"

# ── iCache: size × block size ──────────────────────────────────────────
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

# ── dCache: size (block=16B fixed) ─────────────────────────────────────
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

# ── BPU: bimodal with varying BTB size ─────────────────────────────────
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
# sdram/sram: SoC-mode calibrated timing
# freq-mhz=1000: 1 GHz
default_conf = {
    "stbuf-entries":   "0",
    "sram-lat":        "1",
    "freq-mhz":        "1000",
}
