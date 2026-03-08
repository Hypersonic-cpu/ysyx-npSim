#!/usr/bin/env python3
"""RV32IM calibration error heatmap.

Reads sim results from simout/rv32im-cal-{mode}-{mhz}/ and RTL ground truth
from ccout/rv32im_cal_results.json, then draws per-sweep error heatmaps.

Usage:
  python3 visual/plot_rv32im_cal.py
  python3 visual/plot_rv32im_cal.py \
      --rtl-json ../npc/ccout/rv32im_cal_results.json \
      --outfile  visual/plots/rv32im-cal/error_heatmap.png
"""

import argparse
import json
import os
import sys
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import TwoSlopeNorm

NPSIM_HOME = Path(os.environ.get(
    "NPSIM_HOME", Path(__file__).resolve().parent.parent))
NPC_HOME = Path(os.environ.get("NPC_HOME", NPSIM_HOME.parent / "npc"))

IC_SIZES  = [512, 1024, 2048, 4096]
IC_BLKS   = [16, 32]
DC_SIZES  = [256, 512, 1024, 2048]
BP_SIZES  = [128, 256, 512]
BTB_SIZES = [64, 128, 256]


def load_sim_ipc(simout_dir: Path, tag: str) -> float | None:
    sf = simout_dir / tag / "stats.json"
    if not sf.exists():
        return None
    d = json.load(open(sf))
    s = d.get("stats0", d.get("stats1", {}))
    return s.get("Core", {}).get("ipc")


def err_pct(sim_ipc, rtl_ipc) -> float | None:
    if sim_ipc is None or rtl_ipc is None:
        return None
    return (sim_ipc - rtl_ipc) / rtl_ipc * 100


def draw_panel(ax, data: np.ndarray, row_labels, col_labels,
               vmax: float, title: str, fmt: str = "{:+.1f}%",
               bound: float | None = None):
    """Draw one heatmap panel."""
    norm = TwoSlopeNorm(vmin=-vmax, vcenter=0, vmax=vmax)
    im = ax.imshow(data, cmap="RdYlGn_r", norm=norm,
                   aspect="auto")
    ax.set_xticks(range(len(col_labels)))
    ax.set_xticklabels(col_labels, fontsize=8)
    ax.set_yticks(range(len(row_labels)))
    ax.set_yticklabels(row_labels, fontsize=8)
    ax.set_title(title, fontsize=9)
    plt.colorbar(im, ax=ax, shrink=0.8)
    for i in range(data.shape[0]):
        for j in range(data.shape[1]):
            v = data[i, j]
            if np.isnan(v):
                continue
            fail = bound is not None and abs(v) > bound
            col = "black" if abs(v) < vmax * 0.6 else "white"
            txt = fmt.format(v)
            if fail:
                txt = txt + " X"
            ax.text(j, i, txt, ha="center", va="center",
                    fontsize=7, color=col,
                    fontweight="bold" if fail else "normal")


def plot_icache(rtl: dict, simout_dir: Path, mode: str, mhz: int,
                ax, bound: float):
    prefix = f"rv32im-{mode}-{mhz}"
    n_sz, n_blk = len(IC_SIZES), len(IC_BLKS)
    data = np.full((n_sz, n_blk), np.nan)
    for i, sz in enumerate(IC_SIZES):
        for j, blk in enumerate(IC_BLKS):
            rtl_key = f"{prefix}/icache_ic{sz}_ln{blk}"
            sim_tag = f"rv32im-cal-{mode}-{mhz}/ic{sz}_b{blk}"
            sim = load_sim_ipc(simout_dir, sim_tag)
            rtl_v = rtl.get(rtl_key)
            data[i, j] = err_pct(sim, rtl_v) or np.nan
    row_lbls = [f"{s}B" for s in IC_SIZES]
    col_lbls = [f"{b}B" for b in IC_BLKS]
    draw_panel(ax, data, row_lbls, col_lbls, vmax=15,
               title=f"iCache err% ({mode.upper()} {mhz}MHz)",
               bound=bound)


def plot_dcache(rtl: dict, simout_dir: Path, mode: str, mhz: int,
                ax, bound: float):
    prefix = f"rv32im-{mode}-{mhz}"
    data = np.full((len(DC_SIZES), 1), np.nan)
    for i, sz in enumerate(DC_SIZES):
        rtl_key = f"{prefix}/dcache_dc{sz}_ln16"
        sim_tag = f"rv32im-cal-{mode}-{mhz}/dc{sz}_b16"
        sim = load_sim_ipc(simout_dir, sim_tag)
        rtl_v = rtl.get(rtl_key)
        data[i, 0] = err_pct(sim, rtl_v) or np.nan
    draw_panel(ax, data, [f"{s}B" for s in DC_SIZES], ["16B"],
               vmax=15,
               title=f"dCache err% ({mode.upper()} {mhz}MHz)",
               bound=bound)


def plot_bpu(rtl: dict, simout_dir: Path, mode: str, mhz: int,
             ax, bound: float):
    prefix = f"rv32im-{mode}-{mhz}"
    data = np.full((len(BP_SIZES), len(BTB_SIZES)), np.nan)
    for i, bp in enumerate(BP_SIZES):
        for j, btb in enumerate(BTB_SIZES):
            rtl_key = f"{prefix}/bpu_bp{bp}_btb{btb}"
            sim_tag = f"rv32im-cal-{mode}-{mhz}/bp{bp}_btb{btb}"
            sim = load_sim_ipc(simout_dir, sim_tag)
            rtl_v = rtl.get(rtl_key)
            data[i, j] = err_pct(sim, rtl_v) or np.nan
    draw_panel(ax, data, [str(e) for e in BP_SIZES],
               [str(t) for t in BTB_SIZES],
               vmax=15,
               title=f"BPU err% ({mode.upper()} {mhz}MHz, bimodal)",
               bound=bound)


def main():
    ap = argparse.ArgumentParser(
        description="RV32IM calibration error heatmap")
    ap.add_argument(
        "--rtl-json", type=str,
        default=str(NPC_HOME / "ccout" / "rv32im_cal_results.json"))
    ap.add_argument(
        "--simout-dir", type=str,
        default=str(NPSIM_HOME / "simout"))
    ap.add_argument(
        "--outfile", type=str,
        default=str(NPSIM_HOME / "visual/plots/rv32im-cal/"
                    "error_heatmap.png"))
    args = ap.parse_args()

    rtl = json.load(open(args.rtl_json))
    simout = Path(args.simout_dir)

    # Layout: rows = [NPC-icache | NPC-dcache | NPC-bpu]
    #                [SoC500-icache | SoC500-dcache | SoC1000-icache | SoC1000-dcache]
    fig, axes = plt.subplots(2, 4, figsize=(20, 9))
    fig.suptitle(
        "RV32IM npsim vs RTL IPC error  (positive = sim too fast)\n"
        "X = outside error bound  |  NPC <= 15%  |  SoC <= 5%",
        fontsize=10)

    plot_icache(rtl, simout, "npc", 500, axes[0][0], bound=15)
    plot_dcache(rtl, simout, "npc", 500, axes[0][1], bound=15)
    plot_bpu   (rtl, simout, "npc", 500, axes[0][2], bound=15)
    axes[0][3].axis("off")

    plot_icache(rtl, simout, "soc",  500, axes[1][0], bound=5)
    plot_dcache(rtl, simout, "soc",  500, axes[1][1], bound=5)
    plot_icache(rtl, simout, "soc", 1000, axes[1][2], bound=5)
    plot_dcache(rtl, simout, "soc", 1000, axes[1][3], bound=5)

    plt.tight_layout()
    out = Path(args.outfile)
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(str(out), dpi=150, bbox_inches="tight")
    print(f"Saved: {out}", flush=True)


if __name__ == "__main__":
    main()
