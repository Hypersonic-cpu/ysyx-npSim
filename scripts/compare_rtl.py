#!/usr/bin/env python3
"""Compare npSim vs RTL stats for 23-Mar-2026 SoC calibration sweep.

Directory layout expected:
  RTL : npc/ccout/23-Mar-2026-Cal/<bench-mhz>/<suffix>/stats.json
  npSim: npsim/simout/23-Mar-2026-Cal/<bench-mhz>/<suffix>/stats.json
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Dict, Tuple


def pct_err(sim: float, rtl: float) -> float:
    if rtl == 0:
        return 0.0 if sim == 0 else float("inf")
    return abs(sim - rtl) / abs(rtl) * 100.0


def safe_div(a: float, b: float) -> float:
    return a / b if b else 0.0


def load_rtl_stats(path: Path) -> Dict[str, float]:
    d = json.loads(path.read_text())
    pmu = d["pmu"]
    bc = pmu["BlockedCause"]
    ic = pmu.get("L1ICache", {})
    dc = pmu.get("L1DCache", {})
    bp = pmu.get("BranchPred", {})
    ib = pmu.get("InstBreakdown", {})

    insts = float(ib.get("Commit", bc.get("NoStall", 0)))

    i_acc = float(ic.get("samples", ic.get("Hit", 0) + ic.get("Miss", 0)))
    i_hit = float(ic.get("Hit", 0))
    i_miss = float(ic.get("Miss", 0))
    d_acc = float(dc.get("samples", dc.get("Hit", 0) + dc.get("Miss", 0)))
    d_hit = float(dc.get("Hit", 0))
    d_miss = float(dc.get("Miss", 0))

    bp_correct = float(bp.get("Correct", 0))
    bp_btbmiss = float(bp.get("BtbMiss", 0))
    bp_wrongdir = float(bp.get("WrongDir", 0))
    bp_wrongtgt = float(bp.get("WrongTgt", 0))
    bp_total = max(1.0, bp_correct + bp_btbmiss + bp_wrongdir + bp_wrongtgt)

    cyc_total = max(1.0, float(bc.get("samples", 0)))

    return {
        "ipc": float(pmu.get("ipc", 0.0)),
        "insts": insts,
        "cycles": float(bc.get("samples", 0.0)),
        "cyc_NoStall": safe_div(float(bc.get("NoStall", 0)), cyc_total),
        "cyc_NoInst": safe_div(float(bc.get("NoInst", 0)), cyc_total),
        "cyc_LsuStall": safe_div(float(bc.get("LsuStall", 0)), cyc_total),
        "cyc_BranchMispred": safe_div(float(bc.get("BranchMispred", 0)), cyc_total),
        "cyc_RAW": safe_div(float(bc.get("RAW", 0)), cyc_total),
        "i_hit_rate": safe_div(i_hit, max(1.0, i_acc)),
        "i_miss_rate": safe_div(i_miss, max(1.0, i_acc)),
        "d_hit_rate": safe_div(d_hit, max(1.0, d_acc)),
        "d_miss_rate": safe_div(d_miss, max(1.0, d_acc)),
        "bp_frac_correct": safe_div(bp_correct, bp_total),
        "bp_frac_btbmiss": safe_div(bp_btbmiss, bp_total),
        "bp_frac_wrongdir": safe_div(bp_wrongdir, bp_total),
        "bp_frac_wrongtgt": safe_div(bp_wrongtgt, bp_total),
    }


def load_sim_stats(path: Path) -> Dict[str, float]:
    d = json.loads(path.read_text())
    keys = sorted(k for k in d.keys() if k.startswith("stats"))
    if not keys:
        raise ValueError(f"No stats* key in {path}")
    s = d[keys[-1]]

    core = s.get("Core", {})
    bd = core.get("CycBreakdown", {})
    ic = s.get("iCache", {})
    dc = s.get("dCache", s.get("dNoCache", {}))
    bp = s.get("BranchUnit", {})
    bpu_name = str(d.get("config", {}).get("BranchUnit", {}).get("bpu", ""))
    no_bpu = bpu_name.lower() == "nobpu"

    cycles = float(core.get("cycles", 0))
    cyc_total = max(1.0, cycles)

    i_acc = float(ic.get("accesses", 0))
    i_hit = float(ic.get("hits", 0))
    i_miss = float(ic.get("misses", 0))
    d_acc = float(dc.get("accesses", 0))
    d_hit = float(dc.get("hits", 0))
    d_miss = float(dc.get("misses", 0))

    bp_acc = float(bp.get("br_accesses", bp.get("accesses", 0)))
    if no_bpu:
        # NoBPU mode in RTL attributes taken-branch misses under BTB miss.
        bp_btbmiss = float(bp.get("miss_no_target", 0)) + float(
            bp.get("miss_bad_pred", 0)
        )
        bp_wrongdir = 0.0
    else:
        bp_btbmiss = float(bp.get("miss_no_target", 0))
        bp_wrongdir = float(bp.get("miss_bad_pred", 0))
    bp_wrongtgt = float(bp.get("miss_bad_target", 0))
    bp_correct = max(0.0, bp_acc - bp_btbmiss - bp_wrongdir - bp_wrongtgt)
    bp_total = max(1.0, bp_correct + bp_btbmiss + bp_wrongdir + bp_wrongtgt)

    return {
        "ipc": float(core.get("ipc", 0.0)),
        "insts": float(core.get("insts", 0.0)),
        "cycles": cycles,
        "cyc_NoStall": safe_div(float(bd.get("NoStall", 0)), cyc_total),
        "cyc_NoInst": safe_div(float(bd.get("NoInst", 0)), cyc_total),
        "cyc_LsuStall": safe_div(float(bd.get("LsuStall", 0)), cyc_total),
        "cyc_BranchMispred": safe_div(float(bd.get("BranchMispred", 0)), cyc_total),
        "cyc_RAW": safe_div(float(bd.get("RAW", 0)), cyc_total),
        "i_hit_rate": safe_div(i_hit, max(1.0, i_acc)),
        "i_miss_rate": safe_div(i_miss, max(1.0, i_acc)),
        "d_hit_rate": safe_div(d_hit, max(1.0, d_acc)),
        "d_miss_rate": safe_div(d_miss, max(1.0, d_acc)),
        "bp_frac_correct": safe_div(bp_correct, bp_total),
        "bp_frac_btbmiss": safe_div(bp_btbmiss, bp_total),
        "bp_frac_wrongdir": safe_div(bp_wrongdir, bp_total),
        "bp_frac_wrongtgt": safe_div(bp_wrongtgt, bp_total),
    }


def collect_stats(root: Path) -> Dict[str, Path]:
    out: Dict[str, Path] = {}
    for sf in root.rglob("stats.json"):
        rel = sf.parent.relative_to(root).as_posix()
        out[rel] = sf
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rtl-root", required=True)
    ap.add_argument("--sim-root", required=True)
    ap.add_argument("--ipc-threshold", type=float, default=5.0)
    ap.add_argument("--other-threshold", type=float, default=10.0)
    ap.add_argument("--report-json", default="")
    args = ap.parse_args()

    rtl_root = Path(args.rtl_root)
    sim_root = Path(args.sim_root)
    rtl_map = collect_stats(rtl_root)
    sim_map = collect_stats(sim_root)
    common = sorted(set(rtl_map) & set(sim_map))

    if not common:
        print("No matching stats found between RTL and npSim roots")
        return 1

    rows = []
    for rel in common:
        rtl = load_rtl_stats(rtl_map[rel])
        sim = load_sim_stats(sim_map[rel])

        cyc_keys = [
            "cyc_NoStall",
            "cyc_NoInst",
            "cyc_LsuStall",
            "cyc_BranchMispred",
            "cyc_RAW",
        ]
        cache_keys = ["i_hit_rate", "i_miss_rate", "d_hit_rate", "d_miss_rate"]
        bp_keys = [
            "bp_frac_correct",
            "bp_frac_btbmiss",
            "bp_frac_wrongdir",
            "bp_frac_wrongtgt",
        ]

        cyc_err = {k: abs(sim[k] - rtl[k]) * 100.0 for k in cyc_keys}
        cache_err = {k: abs(sim[k] - rtl[k]) * 100.0 for k in cache_keys}
        bp_err = {k: abs(sim[k] - rtl[k]) * 100.0 for k in bp_keys}

        err = {
            "ipc_err_pct": pct_err(sim["ipc"], rtl["ipc"]),
            "inst_err_pct": pct_err(sim["insts"], rtl["insts"]),
            "cycles_err_pct": pct_err(sim["cycles"], rtl["cycles"]),
            "cycle_breakdown_max_err_pct": max(cyc_err.values()),
            "cache_max_err_pct": max(cache_err.values()),
            "bp_breakdown_max_err_pct": max(bp_err.values()),
        }
        err.update({f"{k}_err_pct": v for k, v in cyc_err.items()})
        err.update({f"{k}_err_pct": v for k, v in cache_err.items()})
        err.update({f"{k}_err_pct": v for k, v in bp_err.items()})

        pass_ipc = err["ipc_err_pct"] <= args.ipc_threshold
        pass_other = (
            err["inst_err_pct"] <= args.other_threshold
            and err["cycle_breakdown_max_err_pct"] <= args.other_threshold
            and err["cache_max_err_pct"] <= args.other_threshold
            and err["bp_breakdown_max_err_pct"] <= args.other_threshold
        )

        rows.append(
            {
                "tag": rel,
                "rtl": rtl,
                "sim": sim,
                "err": err,
                "pass_ipc": pass_ipc,
                "pass_other": pass_other,
                "pass_all": pass_ipc and pass_other,
            }
        )

    def worst(k: str) -> float:
        return max(r["err"][k] for r in rows)

    summary = {
        "rows": len(rows),
        "pass_all": sum(1 for r in rows if r["pass_all"]),
        "fail_all": sum(1 for r in rows if not r["pass_all"]),
        "worst_ipc_err_pct": worst("ipc_err_pct"),
        "worst_inst_err_pct": worst("inst_err_pct"),
        "worst_cycle_breakdown_err_pct": worst("cycle_breakdown_max_err_pct"),
        "worst_cache_err_pct": worst("cache_max_err_pct"),
        "worst_bp_breakdown_err_pct": worst("bp_breakdown_max_err_pct"),
        "ipc_threshold_pct": args.ipc_threshold,
        "other_threshold_pct": args.other_threshold,
    }

    print("Summary")
    print(f"  matched rows               : {summary['rows']}")
    print(f"  pass all                   : {summary['pass_all']}")
    print(f"  fail all                   : {summary['fail_all']}")
    print(f"  worst IPC err (%)          : {summary['worst_ipc_err_pct']:.3f}")
    print(f"  worst Inst err (%)         : {summary['worst_inst_err_pct']:.3f}")
    print(
        f"  worst cycle breakdown err (%): "
        f"{summary['worst_cycle_breakdown_err_pct']:.3f}"
    )
    print(f"  worst cache err (%)        : {summary['worst_cache_err_pct']:.3f}")
    print(f"  worst BP breakdown err (%) : {summary['worst_bp_breakdown_err_pct']:.3f}")

    if args.report_json:
        rp = Path(args.report_json)
        rp.parent.mkdir(parents=True, exist_ok=True)
        rp.write_text(json.dumps({"summary": summary, "rows": rows}, indent=2))
        print(f"Report: {rp}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
