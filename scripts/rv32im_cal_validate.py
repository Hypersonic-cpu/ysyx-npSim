#!/usr/bin/env python3
"""RV32IM calibration: run npsim sweeps and compare with RTL.

Runs npsim for all calibration configs, reads RTL IPC from
ccout/rv32im_cal_results.json, and reports IPC errors.

Usage:
  python3 scripts/rv32im_cal_validate.py
  python3 scripts/rv32im_cal_validate.py --rtl-json ../npc/ccout/rv32im_cal_results.json
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed

NPSIM_HOME = Path(os.environ.get(
    "NPSIM_HOME",
    Path(__file__).resolve().parent.parent))
NPC_HOME = Path(os.environ.get("NPC_HOME", NPSIM_HOME.parent / "npc"))
NPSIM_BIN = NPSIM_HOME / "build" / "npsim.elf"

NPC_TRACE = "tests/coremark-rv32im-npc.nptr.zst"
SOC_TRACE = "tests/coremark-rv32im-soc.nptr.zst"

# Default hardware parameters
IC_DEFAULT = {"size": 1024, "blk": 16, "assoc": 1}
DC_DEFAULT = {"size": 1024, "blk": 16, "assoc": 1}

# NPC mode timing (calibrated at 1 GHz; PMemBox is cycle-based,
# so us values must be scaled by 1000/freq to keep cycle count)
NPC_TIMING_CYCLES = {
    "sdram-lat":      43,   # ceil(0.043 * 1000)
    "sdram-burst":    16,   # ceil(0.016 * 1000)
}
NPC_TIMING_FIXED = {
    "ifq-size":       "4",
    "br-pen":         "1",
    "stbuf-entries":  "2",
}

# SoC mode timing (calibrated for RV32IM)
SOC_TIMING = {
    "sdram-lat-us":           "0.060",
    "sdram-burst-us":         "0.020",
    "icache-sdram-extra-us":  "0.039",
    "sram-lat":               "1",
    "stbuf-entries":          "2",
}

# Sweep dimensions
IC_SIZES = [512, 1024, 2048, 4096]
IC_BLKS = [16, 32]
DC_SIZES = [256, 512, 1024, 2048]
DC_BLK = 16
BPU_ENTRIES = [128, 256, 512]
BTB_SIZES = [64, 128, 256]
BPU_RAS = 8


def rtl_tag(mode, sweep, params, mhz):
    """Build RTL tag matching rv32im_cal_sweep.py naming."""
    prefix = f"rv32im-{mode}-{mhz}"
    if sweep == "icache":
        return (f"{prefix}/icache_ic{params['ic_sz']}"
                f"_ln{params['ic_blk']}")
    elif sweep == "dcache":
        return (f"{prefix}/dcache_dc{params['dc_sz']}"
                f"_ln{params['dc_blk']}")
    elif sweep == "bpu":
        return (f"{prefix}/bpu_bp{params['bp_entries']}"
                f"_btb{params['btb_entries']}")
    elif sweep == "default":
        return f"{prefix}/default"
    return f"{prefix}/{sweep}"


def npsim_tag(mode, sweep, params, mhz):
    """Build npsim output directory tag."""
    prefix = f"rv32im-cal-{mode}-{mhz}"
    if sweep == "icache":
        return (f"{prefix}/ic{params['ic_sz']}"
                f"_b{params['ic_blk']}")
    elif sweep == "dcache":
        return (f"{prefix}/dc{params['dc_sz']}"
                f"_b{params['dc_blk']}")
    elif sweep == "bpu":
        return (f"{prefix}/bp{params['bp_entries']}"
                f"_btb{params['btb_entries']}")
    elif sweep == "default":
        return f"{prefix}/default"
    return f"{prefix}/{sweep}"


def build_npsim_cmd(mode, sweep, params, mhz):
    """Build npsim command line."""
    trace = NPC_TRACE if mode == "npc" else SOC_TRACE
    cmd = [str(NPSIM_BIN), str(NPSIM_HOME / trace)]

    # Cache params
    ic_sz = params.get("ic_sz", IC_DEFAULT["size"])
    ic_blk = params.get("ic_blk", IC_DEFAULT["blk"])
    dc_sz = params.get("dc_sz", DC_DEFAULT["size"])
    dc_blk = params.get("dc_blk", DC_DEFAULT["blk"])

    cmd += ["--l1i-size", str(ic_sz)]
    cmd += ["--l1i-blksize", str(ic_blk)]
    cmd += ["--l1i-assoc", str(IC_DEFAULT["assoc"])]
    cmd += ["--l1d-size", str(dc_sz)]
    cmd += ["--l1d-blksize", str(dc_blk)]
    cmd += ["--l1d-assoc", str(DC_DEFAULT["assoc"])]

    # BPU params
    bp_type = params.get("bp_type", "none")
    if bp_type == "bimodal":
        cmd += ["--bpu-type", "bimodal"]
        cmd += ["--bpu-size", str(params["bp_entries"])]
        cmd += ["--btb-size", str(params["btb_entries"])]
        cmd += ["--ras-size", str(BPU_RAS)]
    else:
        cmd += ["--bpu-type", "none"]

    # Timing params
    if mode == "npc":
        # PMemBox latency is cycle-based; convert to us at freq
        lat_us = NPC_TIMING_CYCLES["sdram-lat"] / mhz
        burst_us = NPC_TIMING_CYCLES["sdram-burst"] / mhz
        cmd += ["--sdram-lat-us", f"{lat_us:.6f}"]
        cmd += ["--sdram-burst-us", f"{burst_us:.6f}"]
        for k, v in NPC_TIMING_FIXED.items():
            cmd += [f"--{k}", v]
    else:
        for k, v in SOC_TIMING.items():
            cmd += [f"--{k}", v]

    # Mode and frequency
    cmd += ["--freq-mhz", str(mhz)]
    if mode == "npc":
        cmd.append("--npc-mode")

    tag = npsim_tag(mode, sweep, params, mhz)
    cmd += ["--outdir", tag, "--print-none"]
    return cmd, tag


def run_npsim(mode, sweep, params, mhz):
    """Run one npsim config. Returns (rtl_tag, sim_tag, ipc)."""
    cmd, sim_tag = build_npsim_cmd(mode, sweep, params, mhz)
    r_tag = rtl_tag(mode, sweep, params, mhz)
    try:
        r = subprocess.run(
            cmd, capture_output=True, text=True,
            cwd=str(NPSIM_HOME), timeout=600)
        if r.returncode != 0:
            print(f"[FAIL] {sim_tag}: {r.stderr[:200]}",
                  flush=True)
            return (r_tag, sim_tag, None)
    except Exception as e:
        print(f"[FAIL] {sim_tag}: {e}", flush=True)
        return (r_tag, sim_tag, None)

    sf = NPSIM_HOME / "simout" / sim_tag / "stats.json"
    if not sf.exists():
        print(f"[FAIL] {sim_tag}: no stats", flush=True)
        return (r_tag, sim_tag, None)
    with open(sf) as f:
        d = json.load(f)
    stats = d.get("stats0", d.get("stats1", {}))
    ipc = stats.get("Core", {}).get("ipc")
    return (r_tag, sim_tag, ipc)


def gen_configs(mode, mhz):
    """Generate config list for one mode/freq combo."""
    configs = []

    # 1-a: iCache sweep
    for sz in IC_SIZES:
        for blk in IC_BLKS:
            p = {"ic_sz": sz, "ic_blk": blk, "bp_type": "none"}
            configs.append(("icache", p))

    # 1-b: dCache sweep
    for sz in DC_SIZES:
        p = {"dc_sz": sz, "dc_blk": DC_BLK, "bp_type": "none"}
        configs.append(("dcache", p))

    # 1-c: default
    p = {"bp_type": "none"}
    configs.append(("default", p))

    # 1-d: BPU sweep (NPC only)
    if mode == "npc":
        for bp_e in BPU_ENTRIES:
            for btb_e in BTB_SIZES:
                p = {
                    "bp_type": "bimodal",
                    "bp_entries": bp_e,
                    "btb_entries": btb_e,
                }
                configs.append(("bpu", p))

    return configs


def main():
    ap = argparse.ArgumentParser(
        description="RV32IM calibration validation")
    ap.add_argument(
        "--rtl-json", type=str,
        default=str(NPC_HOME / "ccout" / "rv32im_cal_results.json"),
        help="RTL results JSON from rv32im_cal_sweep.py")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--mode", choices=["npc", "soc", "all"],
                    default="all")
    ap.add_argument("--skip-sim", action="store_true",
                    help="Skip npsim runs, only compare")
    args = ap.parse_args()

    # Load RTL results
    rtl_results = {}
    rtl_path = Path(args.rtl_json)
    if rtl_path.exists():
        with open(rtl_path) as f:
            rtl_results = json.load(f)
        print(f"Loaded {len(rtl_results)} RTL results from "
              f"{rtl_path}")
    else:
        print(f"Warning: RTL results not found at {rtl_path}")
        print("Will run npsim only (no error comparison)")

    # Build run list
    run_list = []
    if args.mode in ("npc", "all"):
        for sweep, params in gen_configs("npc", 500):
            run_list.append(("npc", sweep, params, 500))
    if args.mode in ("soc", "all"):
        for mhz in [500, 1000]:
            for sweep, params in gen_configs("soc", mhz):
                run_list.append(("soc", sweep, params, mhz))

    # Run npsim
    sim_results = {}
    if not args.skip_sim:
        print(f"\nRunning {len(run_list)} npsim configs "
              f"(jobs={args.jobs})...")
        with ThreadPoolExecutor(
                max_workers=args.jobs) as pool:
            futs = {}
            for mode, sweep, params, mhz in run_list:
                f = pool.submit(
                    run_npsim, mode, sweep, params, mhz)
                futs[f] = (mode, sweep, params, mhz)
            for f in as_completed(futs):
                r_tag, s_tag, ipc = f.result()
                if ipc is not None:
                    sim_results[r_tag] = ipc
    else:
        for mode, sweep, params, mhz in run_list:
            _, sim_tag = build_npsim_cmd(mode, sweep, params, mhz)
            r_tag = rtl_tag(mode, sweep, params, mhz)
            sf = NPSIM_HOME / "simout" / sim_tag / "stats.json"
            if sf.exists():
                with open(sf) as f:
                    d = json.load(f)
                stats = d.get("stats0", d.get("stats1", {}))
                sim_results[r_tag] = stats.get("Core", {}).get(
                    "ipc")

    # Error analysis
    NPC_BOUND = 0.15
    SOC_BOUND = 0.05

    print(f"\n{'='*72}")
    print("  CALIBRATION RESULTS")
    print(f"{'='*72}")

    sections = []
    if args.mode in ("npc", "all"):
        sections.append(("NPC 500MHz", "npc", [500], NPC_BOUND))
    if args.mode in ("soc", "all"):
        sections.append(("SoC 500MHz", "soc", [500], SOC_BOUND))
        sections.append(("SoC 1000MHz", "soc", [1000], SOC_BOUND))

    all_pass = True
    for title, mode, freqs, bound in sections:
        print(f"\n--- {title} (bound={bound*100:.0f}%) ---")
        print(f"{'Config':40s} {'Sim':>8s} {'RTL':>8s} "
              f"{'Err%':>8s} {'Pass':>5s}")
        print("-" * 72)

        for mhz in freqs:
            for sweep, params in gen_configs(mode, mhz):
                r_tag = rtl_tag(mode, sweep, params, mhz)
                sim_ipc = sim_results.get(r_tag)
                rtl_ipc = rtl_results.get(r_tag)

                label = r_tag.split("/", 1)[1] if "/" in r_tag \
                    else r_tag
                sim_s = f"{sim_ipc:.4f}" if sim_ipc else "N/A"
                rtl_s = f"{rtl_ipc:.4f}" if rtl_ipc else "N/A"

                if sim_ipc and rtl_ipc:
                    err = abs(sim_ipc - rtl_ipc) / rtl_ipc
                    passed = err <= bound
                    if not passed:
                        all_pass = False
                    err_s = f"{err*100:+.2f}%"
                    pass_s = "OK" if passed else "FAIL"
                else:
                    err_s = "N/A"
                    pass_s = "N/A"

                print(f"{label:40s} {sim_s:>8s} {rtl_s:>8s} "
                      f"{err_s:>8s} {pass_s:>5s}")

    print(f"\n{'='*72}")
    if all_pass:
        print("  ALL CONFIGS PASSED")
    else:
        print("  SOME CONFIGS FAILED - calibration needed")
    print(f"{'='*72}")

    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
