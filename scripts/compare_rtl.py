#!/usr/bin/env python3
"""Compare npsim vs RTL stats for SoC calibration.

Reads RTL stats from $NPC_HOME/ccout/<rtl-prefix>/*/stats.json
and npsim stats from simout/<sim-prefix>/*/stats.json.
Matches configs by canonical suffix (l1i-*_l1d-*[_bpu-*]).

Usage:
  python3 scripts/compare_rtl.py \
      --rtl-dir $NPC_HOME/ccout/rv32im_soc_cal_1000MHz \
      --sim-dir simout/soc-cal-cm2
"""

import argparse
import json
import re
import sys
from pathlib import Path


def extract_suffix(tag):
    """Extract canonical suffix starting from l1i-."""
    m = re.search(r'(l1i-.*)', tag)
    return m.group(1) if m else tag


def load_rtl_stats(stats_file):
    """Load stats from RTL JSON (pmu format)."""
    with open(stats_file) as f:
        d = json.load(f)
    pmu = d["pmu"]
    bc = pmu["BlockedCause"]
    total = bc["samples"]
    return {
        "ipc": pmu["ipc"],
        "cycles": total,
        "NoStall": bc["NoStall"],
        "NoInst": bc["NoInst"],
        "LsuStall": bc["LsuStall"],
        "BrMispred": bc["BranchMispred"],
        "RAW": bc["RAW"],
        "iCache_miss": pmu.get("L1ICache", {}).get("Miss", 0),
        "iCache_hit": pmu.get("L1ICache", {}).get("Hit", 0),
        "dCache_miss": pmu.get("L1DCache", {}).get("Miss", 0),
        "dCache_hit": pmu.get("L1DCache", {}).get("Hit", 0),
        "bp_correct": pmu.get("BranchPred", {}).get("Correct", 0),
        "bp_btbmiss": pmu.get("BranchPred", {}).get("BtbMiss", 0),
        "bp_wrongdir": pmu.get("BranchPred", {}).get("WrongDir", 0),
        "bp_wrongtgt": pmu.get("BranchPred", {}).get("WrongTgt", 0),
    }


def load_sim_stats(stats_file):
    """Load stats from npsim JSON."""
    with open(stats_file) as f:
        d = json.load(f)
    # Find the stats section (stats0, stats1, ...)
    for key in sorted(d.keys()):
        if key.startswith("stats") and "Core" in d[key]:
            core = d[key]["Core"]
            bd = core["CycBreakdown"]
            ic = d[key].get("iCache", {})
            dc = d[key].get("dCache", d[key].get("dNoCache", {}))
            bp = d[key].get("BranchUnit", {})
            return {
                "ipc": core["ipc"],
                "cycles": core["cycles"],
                "NoStall": bd["NoStall"],
                "NoInst": bd["NoInst"],
                "LsuStall": bd["LsuStall"],
                "BrMispred": bd["BranchMispred"],
                "RAW": bd["RAW"],
                "iCache_miss": ic.get("misses", 0),
                "iCache_hit": ic.get("hits", 0),
                "dCache_miss": dc.get("misses", 0),
                "dCache_hit": dc.get("hits", 0),
                "bp_correct": bp.get("accesses", 0)
                              - bp.get("misses", 0),
                "bp_btbmiss": bp.get("miss_no_target", 0),
                "bp_wrongdir": bp.get("miss_bad_pred", 0),
                "bp_wrongtgt": bp.get("miss_bad_target", 0),
            }
    raise ValueError(f"Cannot parse npsim stats: {stats_file}")


def pct_error(sim, rtl):
    if rtl == 0:
        return 0.0 if sim == 0 else float('inf')
    return (sim - rtl) / rtl * 100.0


def main():
    ap = argparse.ArgumentParser(
        description="Compare npsim vs RTL stats")
    ap.add_argument("--rtl-dir", required=True,
                    help="RTL stats directory")
    ap.add_argument("--sim-dir", required=True,
                    help="npsim stats directory")
    ap.add_argument("--tolerance", type=float, default=5.0,
                    help="IPC error tolerance (%%)")
    ap.add_argument("--verbose", "-v", action="store_true",
                    help="Show detailed stall breakdown")
    args = ap.parse_args()

    rtl_dir = Path(args.rtl_dir)
    sim_dir = Path(args.sim_dir)

    # Build suffix->path maps
    rtl_map = {}
    for sf in rtl_dir.rglob("stats.json"):
        tag = sf.parent.name
        suffix = extract_suffix(tag)
        rtl_map[suffix] = sf

    sim_map = {}
    for sf in sim_dir.rglob("stats.json"):
        tag = sf.parent.name
        suffix = extract_suffix(tag)
        sim_map[suffix] = sf

    common = sorted(set(rtl_map) & set(sim_map))
    if not common:
        print("No matching configs found.")
        print(f"RTL suffixes ({len(rtl_map)}):")
        for s in sorted(rtl_map):
            print(f"  {s}")
        print(f"Sim suffixes ({len(sim_map)}):")
        for s in sorted(sim_map):
            print(f"  {s}")
        sys.exit(1)

    # Header
    cols = ["IPC_err", "Cycles_err"]
    if args.verbose:
        cols += ["NoInst%", "LsuStl%", "BrMis%", "RAW%",
                 "iC_miss", "dC_miss"]
    hdr = f"{'Config':55s}"
    for c in cols:
        hdr += f" {c:>10s}"
    hdr += "  Status"
    print(hdr)
    print("-" * len(hdr))

    pass_count = 0
    fail_count = 0

    for suffix in common:
        rtl = load_rtl_stats(rtl_map[suffix])
        sim = load_sim_stats(sim_map[suffix])

        ipc_err = pct_error(sim["ipc"], rtl["ipc"])
        cyc_err = pct_error(sim["cycles"], rtl["cycles"])
        ok = abs(ipc_err) <= args.tolerance

        line = f"{suffix:55s}"
        line += f" {ipc_err:+9.2f}%"
        line += f" {cyc_err:+9.2f}%"

        if args.verbose:
            for m in ["NoInst", "LsuStall", "BrMispred", "RAW"]:
                e = pct_error(sim[m], rtl[m])
                line += f" {e:+9.1f}%"
            ic_e = pct_error(sim["iCache_miss"], rtl["iCache_miss"])
            dc_e = pct_error(sim["dCache_miss"], rtl["dCache_miss"])
            line += f" {ic_e:+9.1f}%"
            line += f" {dc_e:+9.1f}%"

        line += f"  {'PASS' if ok else 'FAIL'}"
        print(line)
        if ok:
            pass_count += 1
        else:
            fail_count += 1

    print(f"\n{pass_count} PASS, {fail_count} FAIL "
          f"(IPC tolerance: {args.tolerance}%)")
    print(f"Matched {len(common)}/{len(rtl_map)} RTL configs")


if __name__ == "__main__":
    main()
