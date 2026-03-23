# NPC-mode RV32IM calibration: BPU sweep (bimodal)
# Saturation counter entries x BTB size
# Default iCache (1024B/16B/1-way), default dCache (1024B/16B/1-way),
# NPC mode @ 500 MHz.
# Run with:
#   python3 scripts/sweep_2d.py --conf scripts/sweep_configs/npc_rv32im_bpu.py \
#       --prefix coremark --outdir npc-rv32im-bpu --no-area --jobs 4

trace = "tests/coremark-rv32im-npc.nptr.zst"

axis1 = {
    "name": "bpu_entries",
    "vals": [
        {"bpu-size": "128"},
        {"bpu-size": "256"},
        {"bpu-size": "512"},
    ],
    "labels": ["128", "256", "512"],
}

axis2 = {
    "name": "btb_size",
    "vals": [
        {"btb-size": "64"},
        {"btb-size": "128"},
        {"btb-size": "256"},
    ],
    "labels": ["64", "128", "256"],
}

default_conf = {
    "l1i-size":       "1024",
    "l1i-blksize":    "16",
    "l1i-assoc":      "1",
    "l1d-size":       "1024",
    "l1d-blksize":    "16",
    "l1d-assoc":      "1",
    "bpu-type":       "bimodal",
    "ras-size":       "8",
    "stbuf-entries":  "2",
    "freq-mhz":       "500",
    "npc-mode":       True,
}
