# Sweep (c) equivalent: dCache size vs prefetcher (ideal inst supply)
# Run with:
#   python3 sweep_2d.py --conf scripts/sweep_configs/dcache_vs_pf.py \
#                       --outdir 2d_dcache_pf --fig-type stacked --jobs 4

trace = "tests/coremark-10rnd-vld.nptr.zst"

axis1 = {
    "name": "dcache_size",
    "vals": [
        {"l1d-size": "256B",  "l1d-blksize": "16", "l1d-assoc": "1"},
        {"l1d-size": "512B",  "l1d-blksize": "16", "l1d-assoc": "1"},
        {"l1d-size": "1kB",   "l1d-blksize": "16", "l1d-assoc": "1"},
        {"l1d-size": "4kB",   "l1d-blksize": "16", "l1d-assoc": "1"},
    ],
    "labels": ["256B", "512B", "1kB", "4kB"],
}

axis2 = {
    "name": "dpf",
    "vals": [
        {},                       # no dCache prefetcher
        {"dpf": "stride"},
    ],
    "labels": ["no-pf", "stride"],
}

# Ideal inst supply: 4kB iCache, large BPU/BTB, nextline iPF
default_conf = {
    "l1i-size":   "4kB",
    "l1i-blksize": "16",
    "l1i-assoc":  "2",
    "bpu-type":   "bimodal",
    "bpu-size":   "64",
    "btb-size":   "64",
    "ipf":        "nextline",
}
