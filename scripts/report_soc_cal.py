#!/usr/bin/env python3
"""Summarize SoC calibration error against RTL ground truth."""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import sys
from pathlib import Path


def load_common(repo_root: Path):
    common_py = repo_root / "misc" / "soc_cal_23mar2026_common.py"
    spec = importlib.util.spec_from_file_location("soc_cal_common", common_py)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


def load_json(path: Path) -> dict:
    with open(path) as f:
        return json.load(f)


def percentile(vals: list[float], pct: float) -> float:
    if not vals:
        return float("nan")
    idx = max(0, math.ceil(len(vals) * pct) - 1)
    return vals[idx]


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    npsim_home = repo_root / "npsim"
    npc_home = repo_root / "npc"
    common = load_common(repo_root)

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sim-root", default=str(npsim_home / "simout" / "24-Mar-2026-Cal-r5-full"))
    ap.add_argument("--rtl-root", default=str(npc_home / "ccout" / "23-Mar-2026-Cal"))
    ap.add_argument("--benches", default="cm2,dry2500")
    ap.add_argument("--freqs", default="500,1000")
    ap.add_argument("--limit", type=int, default=8)
    args = ap.parse_args()

    sim_root = Path(args.sim_root)
    rtl_root = Path(args.rtl_root)
    benches = [x.strip() for x in args.benches.split(",") if x.strip()]
    freqs = [int(x.strip()) for x in args.freqs.split(",") if x.strip()]

    cfgs = sorted(
        common.configs_for_groups(common.VALID_GROUPS, dedup=True),
        key=lambda c: common.canonical_suffix(c),
    )

    for bench in benches:
        for mhz in freqs:
            rows: list[tuple[float, str, float, float]] = []
            bench_tag = f"{bench}-{mhz}MHz"
            for cfg in cfgs:
                suffix = common.canonical_suffix(cfg)
                rtl_path = rtl_root / bench_tag / suffix / "stats.json"
                sim_path = sim_root / bench_tag / suffix / "stats.json"
                if not rtl_path.exists() or not sim_path.exists():
                    continue
                rtl = load_json(rtl_path)["pmu"]
                sim = load_json(sim_path)["stats0"]
                rtl_ipc = float(rtl["ipc"])
                sim_ipc = float(sim["Core"]["ipc"])
                err = abs(sim_ipc - rtl_ipc) / rtl_ipc if rtl_ipc else 0.0
                rows.append((err, suffix, rtl_ipc, sim_ipc))

            if not rows:
                print(f"[{bench_tag}] no data")
                continue

            rows.sort(reverse=True)
            errs = sorted(r[0] for r in rows)
            print(f"[{bench_tag}] cases={len(rows)} max={rows[0][0]:.4%} "
                  f"mean={sum(errs) / len(errs):.4%} p95={percentile(errs, 0.95):.4%} "
                  f">5%={sum(e > 0.05 for e in errs)}")
            for err, suffix, rtl_ipc, sim_ipc in rows[: args.limit]:
                print(f"  {err:.4%}  rtl={rtl_ipc:.6f} sim={sim_ipc:.6f}  {suffix}")
            print()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
