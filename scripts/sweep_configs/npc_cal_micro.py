# NPC-mode calibration sweep for MicroBench (train)
# Matches RTL configs in ccout/sweep-microbench-train/

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
    "br-pen":         "1",
    "mem-lat":        "45",
    "mem-bstlat":     "10",
    "ifq-size":       "3",
}
