# SoC-mode calibration sweep (Feb 27): iCache size × line size
# ifq=3, sdram_lat=51, burst_lat=24, sram_lat=1 (calibrated ≤3.3% vs RTL)
trace = "tests/coremark-soc-cal.nptr.zst"

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
    "sdram-lat-us":   "0.051",
    "sdram-burst-us": "0.024",
    "sram-lat":       "1",
    "ifq-size":       "3",
}
