#!/usr/bin/env python3
"""run_soc_cal.py -- Run npsim configs matching RTL sweep.

Generates the exact same config tags as npc/scripts/sweep_rv32im_soc_cal.py
so compare_rtl.py can match them by suffix.

Usage:
  python3 scripts/run_soc_cal.py --mhz 1000 --group all --jobs 4
  python3 scripts/run_soc_cal.py --mhz 500 --group bpu
"""

import argparse
import json
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

NPSIM_HOME = Path(__file__).resolve().parent.parent
NPSIM_BIN = NPSIM_HOME / "build" / "npsim.elf"
SIMOUT = NPSIM_HOME / "simout"
TRACE = "tests/cm2-soc-regen.nptr.zst"

IC_ASSOC = 1
DC_ASSOC = 1
DC_BLK = 16


def canonical_tag(mhz, ic_sz, ic_blk, dc_sz, bpu_type,
                  bht=0, btb=0):
    prefix = f"rv32im_soc_cal_{mhz}MHz"
    s = (f"{prefix}_l1i-{ic_sz}-b{ic_blk}-a{IC_ASSOC}"
         f"_l1d-{dc_sz}-b{DC_BLK}-a{DC_ASSOC}")
    if bpu_type == "none":
        s += "_bpu-none"
    else:
        s += f"_bpu-bimodal-h{bht}-t{btb}"
    return s


def gen_bpu_configs(mhz):
    """NoBPU + Bimodal BPU sweep (fixed 1kB caches)."""
    ic_sz, ic_blk, dc_sz = 1024, 16, 1024
    configs = []
    # NoBPU
    configs.append({
        "tag": canonical_tag(mhz, ic_sz, ic_blk, dc_sz, "none"),
        "l1i-size": str(ic_sz), "l1i-blksize": str(ic_blk),
        "l1d-size": str(dc_sz), "l1d-blksize": str(DC_BLK),
        "bpu-type": "none",
    })
    # Bimodal sweep
    for bht in [128, 256, 1024]:
        for btb in [64, 128, 512]:
            configs.append({
                "tag": canonical_tag(mhz, ic_sz, ic_blk, dc_sz,
                                     "bimodal", bht, btb),
                "l1i-size": str(ic_sz), "l1i-blksize": str(ic_blk),
                "l1d-size": str(dc_sz), "l1d-blksize": str(DC_BLK),
                "bpu-type": "bimodal", "bpu-size": str(bht),
                "btb-size": str(btb), "ras-size": "8",
            })
    return configs


def gen_cache_configs(mhz):
    """iCache x dCache sweep (fixed bimodal h256-t128)."""
    bht, btb = 256, 128
    configs = []
    for ic_sz in [512, 1024, 2048]:
        for ic_blk in [16, 32]:
            for dc_sz in [512, 1024]:
                configs.append({
                    "tag": canonical_tag(mhz, ic_sz, ic_blk, dc_sz,
                                         "bimodal", bht, btb),
                    "l1i-size": str(ic_sz),
                    "l1i-blksize": str(ic_blk),
                    "l1d-size": str(dc_sz),
                    "l1d-blksize": str(DC_BLK),
                    "bpu-type": "bimodal", "bpu-size": str(bht),
                    "btb-size": str(btb), "ras-size": "8",
                })
    return configs


def gen_pipe_configs(mhz):
    """Large cache + NoBPU for pipeline validation."""
    ic_sz, ic_blk, dc_sz = 4096, 16, 2048
    return [{
        "tag": canonical_tag(mhz, ic_sz, ic_blk, dc_sz, "none"),
        "l1i-size": str(ic_sz), "l1i-blksize": str(ic_blk),
        "l1d-size": str(dc_sz), "l1d-blksize": str(DC_BLK),
        "bpu-type": "none",
    }]


# Shared defaults (SoC mode, no store buffer, no prefetcher)
SHARED_DEFAULTS = {
    "stbuf-entries": "0",
    "br-pen": "3",
    "sdram-lat-us": "0.051",
    "sdram-burst-us": "0.024",
    "sram-lat": "1",
    "ifq-size": "3",
    "l1i-pref": "none",
    "l1d-pref": "none",
    "l1i-assoc": str(IC_ASSOC),
    "l1d-assoc": str(DC_ASSOC),
    "bpu-no-predecode": None,
}


def run_one(tag, params, outdir, mhz):
    """Run npsim for one config. Returns (tag, ok, msg)."""
    merged = dict(SHARED_DEFAULTS)
    merged["freq-mhz"] = str(mhz)
    merged.update(params)
    del merged["tag"]  # not an npsim flag

    out_tag = f"{outdir}/{tag}"
    cmd = [str(NPSIM_BIN), str(NPSIM_HOME / TRACE)]
    for k, v in merged.items():
        if v is None:
            cmd.append(f"--{k}")
        else:
            cmd += [f"--{k}", str(v)]
    cmd += ["--outdir", out_tag, "--print-none"]

    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           cwd=str(NPSIM_HOME), timeout=300)
        if r.returncode != 0:
            return tag, False, r.stderr.strip()[-200:]
        return tag, True, ""
    except Exception as e:
        return tag, False, str(e)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--group",
                    choices=["bpu", "cache", "pipe", "all"],
                    default="all")
    ap.add_argument("--mhz", type=int, nargs="+",
                    default=[1000],
                    help="Frequency(s) (default: 1000)")
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()

    if not NPSIM_BIN.exists():
        print(f"ERROR: {NPSIM_BIN} not found. Run make first.")
        sys.exit(1)

    for mhz in args.mhz:
        outdir = f"rv32im_soc_cal_{mhz}MHz"
        configs = []
        if args.group in ("bpu", "all"):
            configs += gen_bpu_configs(mhz)
        if args.group in ("cache", "all"):
            configs += gen_cache_configs(mhz)
        if args.group in ("pipe", "all"):
            configs += gen_pipe_configs(mhz)

        # Deduplicate by tag
        seen = set()
        unique = []
        for c in configs:
            if c["tag"] not in seen:
                seen.add(c["tag"])
                unique.append(c)
        configs = unique

        print(f"\n{'='*60}")
        print(f"  {outdir}: {len(configs)} configs @ {mhz} MHz")
        print(f"{'='*60}")

        with ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futures = {
                ex.submit(run_one, c["tag"], c, outdir, mhz): c["tag"]
                for c in configs
            }
            done = 0
            total = len(configs)
            pass_n = 0
            for fut in as_completed(futures):
                tag, ok, msg = fut.result()
                done += 1
                short = tag.split("_", 4)[-1] if "_" in tag else tag
                status = "ok" if ok else f"FAIL: {msg}"
                print(f"  [{done:>{len(str(total))}}/{total}] "
                      f"{short}: {status}")
                if ok:
                    pass_n += 1

        print(f"\n  {pass_n}/{total} succeeded. "
              f"Results in simout/{outdir}/")


if __name__ == "__main__":
    main()
