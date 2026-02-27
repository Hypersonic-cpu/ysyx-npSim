# NPC-mode calibration sweep (Feb 27): iCache size × line size
# IFQ=4, sdram_lat=43, burst_lat=16 (calibrated to ≤5% vs RTL)
trace = "../am-kernels/benchmarks/coremark/build/coremark-nemu-cal.nptr.zst"

axis1 = {
    "name": "cache_size",
    "vals": [
        {"l1i-size": "128B"},
        {"l1i-size": "256B"},
        {"l1i-size": "512B"},
        {"l1i-size": "1kB"},
        {"l1i-size": "4kB"},
    ],
    "labels": ["128B", "256B", "512B", "1kB", "4kB"],
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
    "br-pen":         "1",
    "sdram-lat":      "43",
    "sdram-burst-lat":"16",
    "ifq-size":       "4",
    "npc-mode":       None,
}
