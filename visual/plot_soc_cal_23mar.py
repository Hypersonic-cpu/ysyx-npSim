#!/usr/bin/env python3
"""Generate figures for the 23-Mar-2026 SoC calibration sweep."""

from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


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


def rtl_sim_pair(rtl_root: Path, sim_root: Path, bench: str, mhz: int, suffix: str):
    bench_tag = f"{bench}-{mhz}MHz"
    rtl = load_json(rtl_root / bench_tag / suffix / "stats.json")["pmu"]
    sim = load_json(sim_root / bench_tag / suffix / "stats.json")["stats0"]
    return rtl, sim


def rel_err_pct(sim_val: float, rtl_val: float) -> float:
    if rtl_val == 0:
        return np.nan
    return abs(sim_val - rtl_val) / rtl_val * 100.0


def rtl_cache_triplet(rtl: dict, kind: str) -> dict[str, float]:
    key = "L1ICache" if kind == "i" else "L1DCache"
    data = rtl[key]
    hits = float(data["Hit"])
    misses = float(data["Miss"])
    return {
        "accesses": hits + misses,
        "hits": hits,
        "misses": misses,
    }


def sim_cache_triplet(sim: dict, kind: str) -> dict[str, float]:
    key = "iCache" if kind == "i" else "dCache"
    data = sim[key]
    return {
        "accesses": float(data["accesses"]),
        "hits": float(data["hits"]),
        "misses": float(data["misses"]),
    }


def rtl_bpu_miss_rate(rtl: dict) -> float:
    data = rtl["BranchPred"]
    total = float(data["Correct"] + data["BtbMiss"] + data["WrongDir"] + data["WrongTgt"])
    if total == 0:
        return np.nan
    return float(data["BtbMiss"] + data["WrongDir"] + data["WrongTgt"]) / total


def sim_bpu_miss_rate(sim: dict) -> float:
    return float(sim["BranchUnit"]["miss_rate"])


def rtl_cycle_breakdown_pct(rtl: dict) -> dict[str, float]:
    data = rtl["BlockedCause"]
    total = float(data["samples"])
    return {
        "NoStall": 100.0 * float(data["NoStall"]) / total,
        "NoInst": 100.0 * float(data["NoInst"]) / total,
        "LsuStall": 100.0 * float(data["LsuStall"]) / total,
        "BranchMispred": 100.0 * float(data["BranchMispred"]) / total,
        "RAW": 100.0 * float(data["RAW"]) / total,
    }


def sim_cycle_breakdown_pct(sim: dict) -> dict[str, float]:
    data = sim["Core"]["CycBreakdown"]
    return {
        "NoStall": 100.0 * float(data["NoStall"]) / float(sim["Core"]["cycles"]),
        "NoInst": 100.0 * float(data["NoInst"]) / float(sim["Core"]["cycles"]),
        "LsuStall": 100.0 * float(data["LsuStall"]) / float(sim["Core"]["cycles"]),
        "BranchMispred": 100.0 * float(data["BranchMispred"]) / float(sim["Core"]["cycles"]),
        "RAW": 100.0 * float(data["RAW"]) / float(sim["Core"]["cycles"]),
    }


def heatmap(ax, grid: np.ndarray, rows: list[str], cols: list[str], title: str, cbar_label: str):
    masked = np.ma.masked_invalid(grid)
    im = ax.imshow(masked, cmap="YlOrRd", aspect="auto")
    ax.set_xticks(range(len(cols)))
    ax.set_xticklabels(cols)
    ax.set_yticks(range(len(rows)))
    ax.set_yticklabels(rows)
    ax.set_title(title, fontsize=10)
    plt.colorbar(im, ax=ax, shrink=0.85, label=cbar_label)
    if np.all(np.isnan(grid)):
        return
    vmax = float(np.nanmax(grid))
    for yi in range(grid.shape[0]):
        for xi in range(grid.shape[1]):
            val = grid[yi, xi]
            if np.isnan(val):
                continue
            color = "white" if vmax > 0 and val > vmax * 0.55 else "black"
            ax.text(xi, yi, f"{val:.2f}", ha="center", va="center", fontsize=8, color=color)


def plot_cache_group(common, rtl_root: Path, sim_root: Path, outdir: Path,
                     bench: str, mhz: int, group: str):
    cfgs = [cfg for cfg in common.configs_for_groups([group], dedup=False)]
    if group == "00":
        rows = [512, 1024, 2048]
        cols = [1, 2, 4]
        kind = "i"
        title_prefix = "iCache Size x Assoc"
        value_of = lambda c: (c.l1i_size, c.l1i_assoc)
    elif group == "01":
        rows = [1024, 2048]
        cols = [1, 4]
        kind = "d"
        title_prefix = "dCache Size x Assoc"
        value_of = lambda c: (c.l1d_size, c.l1d_assoc)
    elif group == "03":
        rows = [1024]
        cols = [16, 32]
        kind = "i"
        title_prefix = "iCache 1kB Line Size"
        value_of = lambda c: (c.l1i_size, c.l1i_blk)
    else:
        raise ValueError(group)

    metrics = ("accesses", "hits", "misses")
    grids = {m: np.full((len(rows), len(cols)), np.nan) for m in metrics}
    cfg_map = {value_of(cfg): cfg for cfg in cfgs}

    for r_idx, row in enumerate(rows):
        for c_idx, col in enumerate(cols):
            cfg = cfg_map.get((row, col))
            if cfg is None:
                continue
            suffix = common.canonical_suffix(cfg)
            rtl, sim = rtl_sim_pair(rtl_root, sim_root, bench, mhz, suffix)
            rtl_vals = rtl_cache_triplet(rtl, kind)
            sim_vals = sim_cache_triplet(sim, kind)
            for metric in metrics:
                grids[metric][r_idx, c_idx] = rel_err_pct(sim_vals[metric], rtl_vals[metric])

    fig, axes = plt.subplots(1, 3, figsize=(13, 4.5), constrained_layout=True)
    row_labels = ["1kB" if r == 1024 else "2kB" if r == 2048 else f"{r}B" for r in rows]
    col_labels = ["16B" if group == "03" and c == 16 else "32B" if group == "03" and c == 32 else f"a{c}" for c in cols]
    for ax, metric in zip(axes, metrics):
        heatmap(
            ax,
            grids[metric],
            row_labels,
            col_labels,
            f"{title_prefix}: {metric}",
            "abs rel err (%)",
        )
    fig.suptitle(f"{bench} @ {mhz}MHz: cache breakdown error ({group})", fontsize=12)
    path = outdir / f"{bench}-{mhz}MHz-group{group}-cache-breakdown.png"
    fig.savefig(path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def plot_bpu_group(common, rtl_root: Path, sim_root: Path, outdir: Path,
                   bench: str, mhz: int):
    cfgs = common.configs_for_groups(["04"], dedup=False)
    rows = [64, 512, 1024]
    cols = [128, 256, 512]
    grid = np.full((len(rows), len(cols)), np.nan)
    cfg_map = {(cfg.bp_entries, cfg.btb_entries): cfg for cfg in cfgs}
    for r_idx, bp_entries in enumerate(rows):
        for c_idx, btb_entries in enumerate(cols):
            cfg = cfg_map[(bp_entries, btb_entries)]
            suffix = common.canonical_suffix(cfg)
            rtl, sim = rtl_sim_pair(rtl_root, sim_root, bench, mhz, suffix)
            grid[r_idx, c_idx] = abs(sim_bpu_miss_rate(sim) - rtl_bpu_miss_rate(rtl)) * 100.0

    fig, ax = plt.subplots(1, 1, figsize=(5.8, 4.4), constrained_layout=True)
    heatmap(
        ax,
        grid,
        [str(x) for x in rows],
        [str(x) for x in cols],
        f"{bench} @ {mhz}MHz: BPU miss-rate abs error",
        "abs miss-rate diff (pp)",
    )
    ax.set_ylabel("BPU entries")
    ax.set_xlabel("BTB entries")
    path = outdir / f"{bench}-{mhz}MHz-group04-bpu-missrate-abs.png"
    fig.savefig(path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def short_label(cfg) -> str:
    if cfg.group == "00":
        return f"i{cfg.l1i_size//512}x-a{cfg.l1i_assoc}"
    if cfg.group == "01":
        return f"d{cfg.l1d_size//1024}k-a{cfg.l1d_assoc}"
    if cfg.group == "03":
        return f"i1k-b{cfg.l1i_blk}"
    if cfg.group == "04":
        return f"bp{cfg.bp_entries}-t{cfg.btb_entries}"
    if cfg.group == "05":
        return f"nbp-i{cfg.l1i_size//1024 if cfg.l1i_size >= 1024 else cfg.l1i_size}a{cfg.l1i_assoc}"
    return cfg.group


def plot_cycle_breakdown(common, rtl_root: Path, sim_root: Path, outdir: Path,
                         bench: str, mhz: int):
    cfgs = sorted(
        common.configs_for_groups(common.VALID_GROUPS, dedup=True),
        key=lambda c: (c.group, common.canonical_suffix(c)),
    )
    cats = ["NoStall", "NoInst", "LsuStall", "BranchMispred", "RAW"]
    colors = {
        "NoStall": "#9ecae1",
        "NoInst": "#fdd0a2",
        "LsuStall": "#fdae6b",
        "BranchMispred": "#fb6a4a",
        "RAW": "#807dba",
    }
    labels = [short_label(cfg) for cfg in cfgs]
    x = np.arange(len(cfgs))
    width = 0.42

    fig, ax = plt.subplots(1, 1, figsize=(19, 6.2), constrained_layout=True)
    rtl_bottom = np.zeros(len(cfgs))
    sim_bottom = np.zeros(len(cfgs))
    for cat in cats:
        rtl_vals = []
        sim_vals = []
        for cfg in cfgs:
            suffix = common.canonical_suffix(cfg)
            rtl, sim = rtl_sim_pair(rtl_root, sim_root, bench, mhz, suffix)
            rtl_vals.append(rtl_cycle_breakdown_pct(rtl)[cat])
            sim_vals.append(sim_cycle_breakdown_pct(sim)[cat])
        ax.bar(x - width / 2, rtl_vals, width, bottom=rtl_bottom,
               color=colors[cat], edgecolor="black", linewidth=0.2)
        ax.bar(x + width / 2, sim_vals, width, bottom=sim_bottom,
               color=colors[cat], edgecolor="black", linewidth=0.2, alpha=0.7)
        rtl_bottom += np.array(rtl_vals)
        sim_bottom += np.array(sim_vals)

    ax.set_ylim(0, 100)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=70, ha="right", fontsize=8)
    ax.set_ylabel("cycle share (%)")
    ax.set_title(f"{bench} @ {mhz}MHz: cycle breakdown (RTL left, npSim right)")
    handles = [plt.Rectangle((0, 0), 1, 1, color=colors[cat]) for cat in cats]
    ax.legend(handles, cats, ncols=len(cats), fontsize=8, loc="upper center")
    path = outdir / f"{bench}-{mhz}MHz-cycle-breakdown.png"
    fig.savefig(path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    npsim_home = repo_root / "npsim"
    npc_home = repo_root / "npc"
    common = load_common(repo_root)

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sim-root", default=str(npsim_home / "simout" / "24-Mar-2026-Cal-r5-full"))
    ap.add_argument("--rtl-root", default=str(npc_home / "ccout" / "23-Mar-2026-Cal"))
    ap.add_argument("--outdir", default=str(npsim_home / "visual" / "plots" / "23-Mar-2026-Cal"))
    ap.add_argument("--benches", default="cm2,dry2500")
    ap.add_argument("--freqs", default="500,1000")
    args = ap.parse_args()

    sim_root = Path(args.sim_root)
    rtl_root = Path(args.rtl_root)
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    benches = [x.strip() for x in args.benches.split(",") if x.strip()]
    freqs = [int(x.strip()) for x in args.freqs.split(",") if x.strip()]

    for bench in benches:
        for mhz in freqs:
            for group in ("00", "01", "03"):
                plot_cache_group(common, rtl_root, sim_root, outdir, bench, mhz, group)
            plot_bpu_group(common, rtl_root, sim_root, outdir, bench, mhz)
            plot_cycle_breakdown(common, rtl_root, sim_root, outdir, bench, mhz)

    print(f"Saved figures to {outdir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
