# Sweep (b) equivalent: branch predictor type vs iCache prefetcher
# Run with:
#   python3 sweep_2d.py --conf scripts/sweep_configs/bpu_vs_pf.py \
#                       --outdir 2d_bpu_pf --fig-type stacked --jobs 4

trace = "tests/coremark-10rnd-vld.nptr.zst"

axis1 = {
    "name": "bpu_type",
    "vals": [
        {"bpu-type": "none"},
        {"bpu-type": "bimodal",    "bpu-size": "64", "btb-size": "64"},
        {"bpu-type": "gshare",     "bpu-size": "64", "btb-size": "64"},
        {"bpu-type": "tournament", "bpu-size": "64", "btb-size": "64"},
        {"bpu-type": "alwaystaken"},
        {"bpu-type": "btfnt"},
    ],
    "labels": ["none", "bimodal", "gshare", "tournament", "alwaystaken", "btfnt"],
}

axis2 = {
    "name": "ipf",
    "vals": [
        {},                        # no iCache prefetcher
        {"ipf": "nextline"},
        {"ipf": "stride"},
        {"ipf": "tagged"},
    ],
    "labels": ["no-pf", "nextline", "stride", "tagged"],
}

default_conf = {
    "l1i-size":   "512B",
    "l1i-blksize": "16",
    "l1i-assoc":  "1",
    "br-pen":     "9",
}
