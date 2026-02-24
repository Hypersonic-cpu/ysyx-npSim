#!/usr/bin/env python3
"""Plot heatmap comparing npsim vs RTL IPC across cache configurations.

Reads RTL results from $NPC_HOME/ccout/sweep-cache/ and npsim results
from simout/sweep-rtl-cmp/. Generates a 2D heatmap where cell values
are IPC differences (positive = npsim faster).

Usage:
    python3 scripts/plot_rtl_cmp.py [--outfile visual/plots/rtl_cmp.png]
"""

import argparse
import json
import os
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import TwoSlopeNorm

NPSIM_HOME = Path(os.environ.get("NPSIM_HOME",
                                  Path(__file__).resolve().parent.parent))
NPC_HOME = Path(os.environ["NPC_HOME"])

SIZES = [256, 512, 1024, 2048]
BLKS = [16, 32, 64]


def load_rtl_ipc(size, blk):
    p = NPC_HOME / "ccout" / "sweep-microtrain" / f"l1i_{size}_blk{blk}_assoc1" / "stats.json"
    with open(p) as f:
        return json.load(f)["pmu"]["ipc"]


def load_npsim_ipc(size, blk, sweep_dir="sweep-microtrain"):
    p = NPSIM_HOME / "simout" / sweep_dir / f"l1i_{size}_blk{blk}" / "stats.json"
    with open(p) as f:
        d = json.load(f)
        return d["stats0"]["Core"]["ipc"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--outfile", default="visual/plots/rtl_cmp_mt.png")
    args = parser.parse_args()

    outpath = NPSIM_HOME / args.outfile
    outpath.parent.mkdir(parents=True, exist_ok=True)

    # Build data matrices
    diff_abs = np.zeros((len(SIZES), len(BLKS)))
    diff_pct = np.zeros((len(SIZES), len(BLKS)))
    rtl_ipc = np.zeros((len(SIZES), len(BLKS)))
    npsim_ipc = np.zeros((len(SIZES), len(BLKS)))

    for i, size in enumerate(SIZES):
        for j, blk in enumerate(BLKS):
            r = load_rtl_ipc(size, blk)
            n = load_npsim_ipc(size, blk)
            rtl_ipc[i, j] = r
            npsim_ipc[i, j] = n
            diff_abs[i, j] = n - r
            diff_pct[i, j] = (n - r) / r * 100

    # --- Plot ---
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))

    # Heatmap 1: absolute IPC difference
    vmax = np.max(np.abs(diff_abs))
    norm1 = TwoSlopeNorm(vmin=-vmax, vcenter=0, vmax=vmax)
    im1 = ax1.imshow(diff_abs, cmap="RdYlGn", norm=norm1, aspect="auto")
    ax1.set_xticks(range(len(BLKS)))
    ax1.set_xticklabels([str(b) for b in BLKS])
    ax1.set_yticks(range(len(SIZES)))
    ax1.set_yticklabels([str(s) for s in SIZES])
    ax1.set_xlabel("Block Size (bytes)")
    ax1.set_ylabel("iCache Size (bytes)")
    ax1.set_title("IPC Difference (npSim − RTL)")
    for i in range(len(SIZES)):
        for j in range(len(BLKS)):
            ax1.text(j, i, f"{diff_abs[i,j]:+.4f}\n"
                     f"({npsim_ipc[i,j]:.4f} vs\n {rtl_ipc[i,j]:.4f})",
                     ha="center", va="center", fontsize=8,
                     color="black")
    fig.colorbar(im1, ax=ax1, label="ΔIPC")

    # Heatmap 2: percentage difference
    vmax2 = np.max(np.abs(diff_pct))
    norm2 = TwoSlopeNorm(vmin=-vmax2, vcenter=0, vmax=vmax2)
    im2 = ax2.imshow(diff_pct, cmap="RdYlGn", norm=norm2, aspect="auto")
    ax2.set_xticks(range(len(BLKS)))
    ax2.set_xticklabels([str(b) for b in BLKS])
    ax2.set_yticks(range(len(SIZES)))
    ax2.set_yticklabels([str(s) for s in SIZES])
    ax2.set_xlabel("Block Size (bytes)")
    ax2.set_ylabel("iCache Size (bytes)")
    ax2.set_title("IPC Difference % (npSim − RTL) / RTL")
    for i in range(len(SIZES)):
        for j in range(len(BLKS)):
            ax2.text(j, i, f"{diff_pct[i,j]:+.1f}%",
                     ha="center", va="center", fontsize=11,
                     fontweight="bold", color="black")
    fig.colorbar(im2, ax=ax2, label="ΔIPC %")

    fig.suptitle("npSim vs RTL IPC Comparison — MicroTrain (assoc=1, NoBPU,\n"
                 "mem_lat=42, bst_lat=10, stbuf=2, pf=7)",
                 fontsize=12, fontweight="bold")
    fig.tight_layout()
    fig.savefig(str(outpath), dpi=150, bbox_inches="tight")
    print(f"Saved to {outpath}")


if __name__ == "__main__":
    main()
