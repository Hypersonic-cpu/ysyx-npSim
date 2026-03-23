#!/usr/bin/env python3
"""npSim sweep for SoC calibration matrix (23-Mar-2026).

This script is npSim-only and runs in npsim/.
It never triggers RTL compile/run.

Trace inputs:
  tests/cm2-im-optklib2.nptr.zst
  tests/dry2500-im-optklib2.nptr.zst

Output layout:
  npsim/simout/23-Mar-2026-Cal/{cm2,dry2500}-{500,1000}MHz/<suffix>/stats.json
"""

from __future__ import annotations

import argparse
import importlib.util
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Dict, List, Tuple


def load_common(repo_root: Path):
    common_py = repo_root / "misc" / "soc_cal_23mar2026_common.py"
    if not common_py.exists():
        raise FileNotFoundError(f"Missing shared config: {common_py}")
    spec = importlib.util.spec_from_file_location("soc_cal_common", common_py)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


def ensure_exists(path: Path, what: str) -> None:
    if not path.exists():
        raise FileNotFoundError(f"{what} not found: {path}")


def parse_csv_ints(s: str) -> List[int]:
    vals = [x.strip() for x in s.split(",") if x.strip()]
    return [int(x) for x in vals]


def parse_csv_strs(s: str) -> List[str]:
    return [x.strip() for x in s.split(",") if x.strip()]


def run_one(
    npsim_home: str,
    npsim_bin: str,
    trace_file: str,
    out_tag: str,
    params: Dict[str, str | None],
    timeout_s: int,
) -> Tuple[str, bool, str]:
    stats = Path(npsim_home) / "simout" / out_tag / "stats.json"
    if stats.exists():
        return out_tag, True, "cached"

    cmd = [npsim_bin, trace_file]
    for k, v in params.items():
        if v is None:
            cmd.append(f"--{k}")
        else:
            cmd += [f"--{k}", str(v)]
    cmd += ["--outdir", out_tag, "--print-none"]

    p = subprocess.run(
        cmd,
        text=True,
        capture_output=True,
        timeout=timeout_s,
        cwd=npsim_home,
    )
    if p.returncode != 0 or not stats.exists():
        return out_tag, False, (p.stdout + p.stderr)[-1200:]
    return out_tag, True, "ok"


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    npsim_home = repo_root / "npsim"
    common = load_common(repo_root)

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--groups", default=common.default_groups_csv())
    ap.add_argument("--freqs", default="500,1000")
    ap.add_argument("--benches", default="cm2,dry2500")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--out-root", default="23-Mar-2026-Cal")
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--limit-configs", type=int, default=0)
    ap.add_argument("--stbuf-entries", type=int, default=0)
    ap.add_argument("--sram-lat", type=int, default=1)
    args = ap.parse_args()

    npsim_bin = npsim_home / "build" / "npsim.elf"
    ensure_exists(npsim_bin, "npSim binary")

    trace_map = {
        "cm2": npsim_home / "tests" / "cm2-im-optklib2.nptr.zst",
        "dry2500": npsim_home / "tests" / "dry2500-im-optklib2.nptr.zst",
    }

    groups = common.parse_groups(args.groups)
    freqs = parse_csv_ints(args.freqs)
    benches = parse_csv_strs(args.benches)

    for b in benches:
        if b not in trace_map:
            raise ValueError(f"Unsupported bench '{b}', choose from {sorted(trace_map)}")
        ensure_exists(trace_map[b], f"trace ({b})")

    cfgs = common.configs_for_groups(groups, dedup=True)
    cfgs = sorted(cfgs, key=lambda c: common.canonical_suffix(c))
    if args.limit_configs > 0:
        cfgs = cfgs[: args.limit_configs]

    base_params: Dict[str, str | None] = {
        "stbuf-entries": str(args.stbuf_entries),
        "sram-lat": str(args.sram_lat),
        "l1i-pref": "none",
        "l1d-pref": "none",
    }

    print(f"Groups: {groups}")
    print(f"Unique HW configs: {len(cfgs)}")
    print(f"Freqs: {freqs}")
    print(f"Benches: {benches}")
    print(f"Output root: npsim/simout/{args.out_root}")

    tasks = []
    for mhz in freqs:
        for cfg in cfgs:
            hw = common.npsim_hw_params(cfg)
            for b in benches:
                tag = f"{args.out_root}/{common.full_tag(b, mhz, cfg)}"
                p = dict(base_params)
                p["freq-mhz"] = str(mhz)
                p.update(hw)
                tasks.append((
                    str(trace_map[b]),
                    tag,
                    p,
                ))

    print(f"Total npSim tasks: {len(tasks)} (jobs={args.jobs})")
    fail = 0
    done = 0

    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {
            ex.submit(
                run_one,
                str(npsim_home),
                str(npsim_bin),
                trace,
                out_tag,
                params,
                args.timeout,
            ): out_tag
            for trace, out_tag, params in tasks
        }
        total = len(futs)
        for fut in as_completed(futs):
            tag, ok, msg = fut.result()
            done += 1
            if not ok:
                fail += 1
            status = "OK" if ok else "FAIL"
            print(f"  [{done}/{total}] {tag}: {status}")
            if not ok:
                print(msg)

    print("\n[Summary]")
    print(f"  Total tasks : {len(tasks)}")
    print(f"  Failed      : {fail}")
    print(f"  Output base : {npsim_home / 'simout' / args.out_root}")
    return 0 if fail == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
