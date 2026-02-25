# SoC-mode calibration sweep for MicroBench (train)
# Matches RTL configs in ccout/sweep-microbench-soc-train/

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
    "br-pen":          "1",
    "sdram-lat":       "55",
    "sdram-burst-lat": "23",
    "ifq-size":        "3",
    "socmode":         None,
    "sram-lat":        "1",
}
