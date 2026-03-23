"""npsim sweep config: SoC calibration against RTL.

Matches RTL sweep_rv32im_soc_cal.py configs exactly.

Three independent sweep groups (run via sweep_2d.py --conf):

  1. BPU sweep: NoBPU + Bimodal BTB={64,128,512} x BHT={128,256,1024}
     (fixed iCache 1kB/16B, dCache 1kB/16B)
  2. Cache sweep: iCache {512,1024,2048} x blk {16,32}
     dCache {512,1024} x blk 16B
     (fixed BPU bimodal h256-t128)
  3. Pipeline sweep: large caches (4kB/16B iCache, 2kB/16B dCache)
     NoBPU (for RAW/forwarding validation)

All use cm2-soc-regen.nptr.zst (2-iter coremark, SoC mode).
"""

trace = "tests/cm2-soc-regen.nptr.zst"

# BPU sweep: varies BPU type/size, fixes caches at 1kB
bpu_axis = {
    "axis1": {
        "name": "bpu_config",
        "vals": [
            {"bpu-type": "none",    "bpu-size": "0",    "btb-size": "0"},
            {"bpu-type": "bimodal", "bpu-size": "128",  "btb-size": "64"},
            {"bpu-type": "bimodal", "bpu-size": "256",  "btb-size": "64"},
            {"bpu-type": "bimodal", "bpu-size": "1024", "btb-size": "64"},
            {"bpu-type": "bimodal", "bpu-size": "128",  "btb-size": "128"},
            {"bpu-type": "bimodal", "bpu-size": "256",  "btb-size": "128"},
            {"bpu-type": "bimodal", "bpu-size": "1024", "btb-size": "128"},
            {"bpu-type": "bimodal", "bpu-size": "128",  "btb-size": "512"},
            {"bpu-type": "bimodal", "bpu-size": "256",  "btb-size": "512"},
            {"bpu-type": "bimodal", "bpu-size": "1024", "btb-size": "512"},
        ],
        "labels": [
            "NoBPU",
            "h128-t64", "h256-t64", "h1024-t64",
            "h128-t128", "h256-t128", "h1024-t128",
            "h128-t512", "h256-t512", "h1024-t512",
        ],
    },
    "fixed": {
        "ras-size": "8",
        "l1i-size": "1kB", "l1i-blksize": "16", "l1i-assoc": "1",
        "l1d-size": "1kB", "l1d-blksize": "16", "l1d-assoc": "1",
    },
}

# iCache sweep (fixed dCache 512B/16B and BPU bimodal h256-t128)
icache_axis = {
    "axis1": {
        "name": "l1i_size",
        "vals": [
            {"l1i-size": "512B"},
            {"l1i-size": "1kB"},
            {"l1i-size": "2kB"},
        ],
        "labels": ["512B", "1kB", "2kB"],
    },
    "axis2": {
        "name": "l1i_blksize",
        "vals": [
            {"l1i-blksize": "16"},
            {"l1i-blksize": "32"},
        ],
        "labels": ["16B", "32B"],
    },
    "fixed": {
        "l1i-assoc": "1",
        "bpu-type": "bimodal", "bpu-size": "256", "btb-size": "128",
        "ras-size": "8",
    },
}

# dCache sweep (fixed iCache and BPU)
dcache_axis = {
    "axis1": {
        "name": "l1d_size",
        "vals": [
            {"l1d-size": "512B"},
            {"l1d-size": "1kB"},
        ],
        "labels": ["512B", "1kB"],
    },
    "fixed": {
        "l1d-blksize": "16", "l1d-assoc": "1",
        "bpu-type": "bimodal", "bpu-size": "256", "btb-size": "128",
        "ras-size": "8",
    },
}

# Shared defaults: SoC mode, no store buffer, no prefetcher
default_conf = {
    "stbuf-entries":    "0",
    "sram-lat":         "1",
    "freq-mhz":         "1000",
    "l1i-pref":         "none",
    "l1d-pref":         "none",
}
