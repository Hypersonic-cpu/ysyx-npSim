#!/usr/bin/env python3
"""compare_rtl.py — Compare RTL and npsim IPC/stalls, generate error heatmap.

Usage:
  python3 scripts/compare_rtl.py \
      --rtl-dir  $NPC_HOME/ccout/sweep-cache \
      --sim-dir  simout/npc-cal \
      --outfile  visual/plots/npc-cal/rtl_cmp.png
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.colors import TwoSlopeNorm

SIZES  = [256, 512, 1024, 4096]
BLKSZ  = [8, 16, 32, 64]
SIZE_LABELS = ["256B", "512B", "1kB", "4kB"]
BLK_LABELS  = ["8B", "16B", "32B", "64B"]


def load_rtl_stats(rtl_dir: Path) -> dict:
    """Load RTL stats from sweep-cache/<subdir>/stats.json.
    Returns {(size, blk): {ipc, BlockedCause, L1ICache}}"""
    result = {}
    for sub in sorted(rtl_dir.iterdir()):
        if not sub.is_dir():
            continue
        m = re.match(r"l1i_(\d+)_blk(\d+)_assoc(\d+)", sub.name)
        if not m:
            continue
        sz, blk, assoc = int(m[1]), int(m[2]), int(m[3])
        if assoc != 1 or sz not in SIZES or blk not in BLKSZ:
            continue
        stats_file = sub / "stats.json"
        if not stats_file.exists() or stats_file.stat().st_size == 0:
            continue
        with open(stats_file) as f:
            try:
                data = json.load(f)
            except json.JSONDecodeError:
                continue
        pmu = data.get("pmu", {})
        bc = pmu.get("BlockedCause", {})
        l1i = pmu.get("L1ICache", {})
        result[(sz, blk)] = {
            "ipc": pmu.get("ipc", 0.0),
            "cycles": bc.get("samples", 1),
            "NoStall": bc.get("NoStall", 0),
            "NoInst": bc.get("NoInst", 0),
            "LsuStall": bc.get("LsuStall", 0),
            "BrMispred": bc.get("BranchMispred", 0),
            "RAW": bc.get("RAW", 0),
            "icache_hits": l1i.get("Hit", 0),
            "icache_misses": l1i.get("Miss", 0),
        }
    return result


def load_sim_stats(sim_dir: Path) -> dict:
    """Load npsim stats from npc-cal/<tag>/stats.json.
    Returns {(size, blk): {ipc, CycBreakdown, iCache}}"""
    result = {}
    for sub in sorted(sim_dir.iterdir()):
        if not sub.is_dir():
            continue
        # Parse tag like cache_size-256B_line_size-16B
        m = re.match(
            r"cache_size-(\d+)([kKMB]+)_line_size-(\d+)B", sub.name)
        if not m:
            continue
        sz_num = int(m[1])
        sz_unit = m[2].lower()
        blk = int(m[3])
        if sz_unit == "kb":
            sz_num *= 1024
        elif sz_unit == "mb":
            sz_num *= 1024 * 1024
        # sz_unit == "b" → keep as is
        if sz_num not in SIZES or blk not in BLKSZ:
            continue
        stats_file = sub / "stats.json"
        if not stats_file.exists():
            continue
        with open(stats_file) as f:
            data = json.load(f)
        s = data.get("stats1", data.get("stats0", {}))
        core = s.get("Core", {})
        bd = core.get("CycBreakdown", {})
        ic = s.get("iCache", {})
        result[(sz_num, blk)] = {
            "ipc": core.get("ipc", 0.0),
            "cycles": core.get("cycles", 1),
            "NoStall": bd.get("NoStall", 0),
            "NoInst": bd.get("NoInst", 0),
            "LsuStall": bd.get("LsuStall", 0),
            "BrMispred": bd.get("BranchMispred", 0),
            "RAW": bd.get("RAW", 0),
            "icache_hits": ic.get("hits", 0),
            "icache_misses": ic.get("misses", 0),
        }
    return result


def make_error_heatmap(rtl, sim, outfile, title=""):
    """Generate IPC error heatmap + stall comparison."""
    n_sizes = len(SIZES)
    n_blks = len(BLKSZ)

    ipc_err = np.full((n_sizes, n_blks), np.nan)
    rtl_ipc = np.full((n_sizes, n_blks), np.nan)
    sim_ipc = np.full((n_sizes, n_blks), np.nan)

    stall_cats = ["NoStall", "NoInst", "LsuStall", "BrMispred", "RAW"]
    stall_err = {c: np.full((n_sizes, n_blks), np.nan) for c in stall_cats}

    for i, sz in enumerate(SIZES):
        for j, blk in enumerate(BLKSZ):
            r = rtl.get((sz, blk))
            s = sim.get((sz, blk))
            if r is None or s is None:
                continue
            rtl_ipc[i, j] = r["ipc"]
            sim_ipc[i, j] = s["ipc"]
            if r["ipc"] > 0:
                ipc_err[i, j] = (s["ipc"] - r["ipc"]) / r["ipc"] * 100
            for c in stall_cats:
                r_pct = r[c] / max(r["cycles"], 1) * 100
                s_pct = s[c] / max(s["cycles"], 1) * 100
                stall_err[c][i, j] = s_pct - r_pct

    # Plot
    fig, axes = plt.subplots(2, 3, figsize=(18, 10))
    fig.suptitle(title or "RTL vs npsim Comparison", fontsize=14)

    # IPC Error heatmap
    ax = axes[0, 0]
    vmax = max(abs(np.nanmin(ipc_err)), abs(np.nanmax(ipc_err)), 1)
    norm = TwoSlopeNorm(vmin=-vmax, vcenter=0, vmax=vmax)
    im = ax.imshow(ipc_err, cmap='RdYlGn_r', norm=norm, aspect='auto')
    ax.set_xticks(range(n_blks))
    ax.set_xticklabels(BLK_LABELS)
    ax.set_yticks(range(n_sizes))
    ax.set_yticklabels(SIZE_LABELS)
    ax.set_xlabel("Block Size")
    ax.set_ylabel("Cache Size")
    ax.set_title("IPC Error (%)")
    for i in range(n_sizes):
        for j in range(n_blks):
            v = ipc_err[i, j]
            if not np.isnan(v):
                ax.text(j, i, f"{v:+.1f}%", ha='center', va='center',
                        fontsize=9, fontweight='bold')
    fig.colorbar(im, ax=ax, shrink=0.8)

    # RTL IPC heatmap
    ax = axes[0, 1]
    im = ax.imshow(rtl_ipc, cmap='viridis', aspect='auto')
    ax.set_xticks(range(n_blks))
    ax.set_xticklabels(BLK_LABELS)
    ax.set_yticks(range(n_sizes))
    ax.set_yticklabels(SIZE_LABELS)
    ax.set_xlabel("Block Size")
    ax.set_title("RTL IPC")
    for i in range(n_sizes):
        for j in range(n_blks):
            v = rtl_ipc[i, j]
            if not np.isnan(v):
                ax.text(j, i, f"{v:.4f}", ha='center', va='center',
                        fontsize=8, color='white')
    fig.colorbar(im, ax=ax, shrink=0.8)

    # Sim IPC heatmap
    ax = axes[0, 2]
    im = ax.imshow(sim_ipc, cmap='viridis', aspect='auto')
    ax.set_xticks(range(n_blks))
    ax.set_xticklabels(BLK_LABELS)
    ax.set_yticks(range(n_sizes))
    ax.set_yticklabels(SIZE_LABELS)
    ax.set_xlabel("Block Size")
    ax.set_title("npsim IPC")
    for i in range(n_sizes):
        for j in range(n_blks):
            v = sim_ipc[i, j]
            if not np.isnan(v):
                ax.text(j, i, f"{v:.4f}", ha='center', va='center',
                        fontsize=8, color='white')
    fig.colorbar(im, ax=ax, shrink=0.8)

    # Stall breakdown comparison (bottom row)
    cats_to_show = ["NoInst", "LsuStall", "BrMispred"]
    for k, cat in enumerate(cats_to_show):
        ax = axes[1, k]
        data = stall_err[cat]
        vmax = max(abs(np.nanmin(data)), abs(np.nanmax(data)), 1)
        norm = TwoSlopeNorm(vmin=-vmax, vcenter=0, vmax=vmax)
        im = ax.imshow(data, cmap='RdBu_r', norm=norm, aspect='auto')
        ax.set_xticks(range(n_blks))
        ax.set_xticklabels(BLK_LABELS)
        ax.set_yticks(range(n_sizes))
        ax.set_yticklabels(SIZE_LABELS)
        ax.set_xlabel("Block Size")
        if k == 0:
            ax.set_ylabel("Cache Size")
        ax.set_title(f"{cat} Error (pp)")
        for i in range(n_sizes):
            for j in range(n_blks):
                v = data[i, j]
                if not np.isnan(v):
                    ax.text(j, i, f"{v:+.1f}", ha='center', va='center',
                            fontsize=8)
        fig.colorbar(im, ax=ax, shrink=0.8)

    plt.tight_layout()
    os.makedirs(os.path.dirname(outfile), exist_ok=True)
    plt.savefig(outfile, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved {outfile}")

    # Print summary table
    print(f"\n{'='*70}")
    print(f"{'Config':>20s} {'RTL IPC':>10s} {'Sim IPC':>10s} {'Error':>10s}")
    print(f"{'='*70}")
    for i, sz in enumerate(SIZES):
        for j, blk in enumerate(BLKSZ):
            r = rtl.get((sz, blk))
            s = sim.get((sz, blk))
            if r is None or s is None:
                print(f"{SIZE_LABELS[i]+'/'+BLK_LABELS[j]:>20s} {'N/A':>10s} {'N/A':>10s} {'N/A':>10s}")
                continue
            err = ipc_err[i, j]
            print(f"{SIZE_LABELS[i]+'/'+BLK_LABELS[j]:>20s} {r['ipc']:10.6f} {s['ipc']:10.6f} {err:+9.2f}%")
    print(f"{'='*70}")

    # Print cache stats comparison
    print(f"\n{'Cache Stats Comparison':>40s}")
    print(f"{'Config':>20s} {'RTL Hits':>10s} {'Sim Hits':>10s} {'RTL Miss':>10s} {'Sim Miss':>10s}")
    for i, sz in enumerate(SIZES):
        for j, blk in enumerate(BLKSZ):
            r = rtl.get((sz, blk))
            s = sim.get((sz, blk))
            if r is None or s is None:
                continue
            print(f"{SIZE_LABELS[i]+'/'+BLK_LABELS[j]:>20s} {r['icache_hits']:>10d} {s['icache_hits']:>10d} {r['icache_misses']:>10d} {s['icache_misses']:>10d}")


def main():
    parser = argparse.ArgumentParser(description="Compare RTL and npsim stats")
    parser.add_argument("--rtl-dir", required=True, help="RTL sweep dir")
    parser.add_argument("--sim-dir", required=True, help="npsim simout sweep dir")
    parser.add_argument("--outfile", default="visual/plots/rtl_cmp.png")
    parser.add_argument("--title", default="")
    args = parser.parse_args()

    npsim_home = Path(os.environ.get("NPSIM_HOME", "."))
    rtl_dir = Path(args.rtl_dir)
    sim_dir = npsim_home / args.sim_dir if not Path(args.sim_dir).is_absolute() else Path(args.sim_dir)

    rtl = load_rtl_stats(rtl_dir)
    sim = load_sim_stats(sim_dir)

    print(f"RTL configs loaded: {len(rtl)}")
    print(f"Sim configs loaded: {len(sim)}")

    if not rtl:
        print("ERROR: No RTL stats found!", file=sys.stderr)
        sys.exit(1)
    if not sim:
        print("ERROR: No npsim stats found!", file=sys.stderr)
        sys.exit(1)

    make_error_heatmap(rtl, sim, args.outfile, args.title)


if __name__ == "__main__":
    main()
