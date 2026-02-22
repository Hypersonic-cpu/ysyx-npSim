#!/usr/bin/env python3
"""Visualize npSim sweep results from sweep_a, sweep_b, sweep_c."""

import json
import os
import sys
import re
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from collections import defaultdict

SIMOUT = os.path.join(os.environ.get("NPSIM_HOME", "."), "simout")
AREAOUT = os.path.join(os.environ.get("NPSIM_HOME", "."), "areaout")
VISOUT = os.path.join(os.environ.get("NPSIM_HOME", "."), "visual", "plots")
os.makedirs(VISOUT, exist_ok=True)


def load_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (json.JSONDecodeError, ValueError):
        return None


def get_stats(data):
    """Return stats1 if exists (after second reset), else stats0."""
    return data.get("stats1", data.get("stats0", {}))


def get_ipc(data):
    s = get_stats(data)
    return s.get("Core", {}).get("ipc", 0)


def get_icache_mr(data):
    s = get_stats(data)
    ic = s.get("iCache", {})
    acc = ic.get("accesses", 1)
    miss = ic.get("misses", 0)
    return miss / max(acc, 1)


def get_stall_breakdown(data):
    s = get_stats(data)
    core = s.get("Core", {})
    bd = core.get("CycBreakdown", core)
    total = core.get("cycles", 1)
    return {
        "NoInst": bd.get("NoInst", 0) / total,
        "LsuStall": bd.get("LsuStall", 0) / total,
        "BrMispred": bd.get("BranchMispred", 0) / total,
        "RAW": bd.get("RAW", 0) / total,
        "Useful": bd.get("NoStall", core.get("NoStall", 0)) / total,
    }


def get_cycle_breakdown(data):
    """Return absolute cycle counts per category."""
    s = get_stats(data)
    core = s.get("Core", {})
    bd = core.get("CycBreakdown", core)
    return {
        "NoInst": bd.get("NoInst", 0),
        "LsuStall": bd.get("LsuStall", 0),
        "BrMispred": bd.get("BranchMispred", 0),
        "RAW": bd.get("RAW", 0),
        "Useful": bd.get("NoStall", core.get("NoStall", 0)),
    }


def get_bp_miss_rate(data):
    s = get_stats(data)
    for key in ["BranchUnit", "Core"]:
        obj = s.get(key, {})
        if "miss_rate" in obj:
            return obj["miss_rate"]
    return 0


def load_area(sweep_subdir, json_basename):
    """Load area_comp.json for a given sweep result."""
    stem = json_basename.replace(".json", "")
    # Primary: areaout/<sweep_subdir>/<stem>/area_comp.json
    area_dir = os.path.join(AREAOUT, sweep_subdir, stem)
    path = os.path.join(area_dir, "area_comp.json")
    if os.path.isfile(path):
        return load_json(path)
    # Fallback: areaout/<stem>/area_comp.json
    area_dir = os.path.join(AREAOUT, stem)
    path = os.path.join(area_dir, "area_comp.json")
    if os.path.isfile(path):
        return load_json(path)
    return None


def get_total_area(area_data):
    """Get total area in um² from area_comp.json."""
    if area_data is None:
        return 0
    return area_data.get("total_area_um2", 0)


def get_area_breakdown(area_data):
    """Get component-wise area breakdown."""
    if area_data is None:
        return {}
    result = {}
    for comp in area_data.get("components", []):
        result[comp["name"]] = comp["total_um2"]
    return result


# ============================================================
# Sweep (a): Cache config sweep — heatmaps
# ============================================================
def plot_sweep_a():
    sweep_dir = os.path.join(SIMOUT, "sweep_a")
    if not os.path.isdir(sweep_dir):
        print("sweep_a not found, skipping")
        return

    traces = ["coremark-10rnd-vld", "micro-train-vld"]
    sizes = [256, 512, 1024, 4096]
    lines = [8, 16, 32, 64]
    assocs = [1, 2]

    for trace in traces:
        short = "CoreMark" if "coremark" in trace else "MicroTrain"
        for assoc in assocs:
            ipc_grid = np.zeros((len(sizes), len(lines)))
            mr_grid = np.zeros((len(sizes), len(lines)))
            for i, sz in enumerate(sizes):
                for j, ln in enumerate(lines):
                    fname = f"{trace}_sz{sz}_ln{ln}_a{assoc}.json"
                    path = os.path.join(sweep_dir, fname)
                    if os.path.isfile(path):
                        d = load_json(path)
                        if d:
                            ipc_grid[i, j] = get_ipc(d)
                            mr_grid[i, j] = get_icache_mr(d)

            fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))
            fig.suptitle(f"{short} — Assoc={assoc}")

            im1 = ax1.imshow(ipc_grid, aspect='auto', cmap='YlGn')
            ax1.set_xticks(range(len(lines)))
            ax1.set_xticklabels([f"{l}B" for l in lines])
            ax1.set_yticks(range(len(sizes)))
            ax1.set_yticklabels([f"{s}B" for s in sizes])
            ax1.set_xlabel("Line Size")
            ax1.set_ylabel("Cache Size")
            ax1.set_title("IPC")
            for yi in range(len(sizes)):
                for xi in range(len(lines)):
                    ax1.text(xi, yi, f"{ipc_grid[yi,xi]:.4f}",
                             ha='center', va='center', fontsize=8)
            plt.colorbar(im1, ax=ax1)

            im2 = ax2.imshow(mr_grid, aspect='auto', cmap='YlOrRd')
            ax2.set_xticks(range(len(lines)))
            ax2.set_xticklabels([f"{l}B" for l in lines])
            ax2.set_yticks(range(len(sizes)))
            ax2.set_yticklabels([f"{s}B" for s in sizes])
            ax2.set_xlabel("Line Size")
            ax2.set_ylabel("Cache Size")
            ax2.set_title("iCache Miss Rate")
            for yi in range(len(sizes)):
                for xi in range(len(lines)):
                    ax2.text(xi, yi, f"{mr_grid[yi,xi]:.4f}",
                             ha='center', va='center', fontsize=8)
            plt.colorbar(im2, ax=ax2)

            plt.tight_layout()
            plt.savefig(os.path.join(VISOUT, f"sweep_a_{short}_a{assoc}.png"),
                        dpi=150)
            plt.close()
            print(f"  Saved sweep_a_{short}_a{assoc}.png")

    # Area heatmap (combined across traces, using coremark)
    trace = "coremark-10rnd-vld"
    short = "CoreMark"
    for assoc in assocs:
        area_grid = np.zeros((len(sizes), len(lines)))
        for i, sz in enumerate(sizes):
            for j, ln in enumerate(lines):
                fname = f"{trace}_sz{sz}_ln{ln}_a{assoc}.json"
                ad = load_area("sweep_a", fname)
                area_grid[i, j] = get_total_area(ad)

        fig, ax = plt.subplots(figsize=(7, 5))
        im = ax.imshow(area_grid, aspect='auto', cmap='OrRd')
        ax.set_xticks(range(len(lines)))
        ax.set_xticklabels([f"{l}B" for l in lines])
        ax.set_yticks(range(len(sizes)))
        ax.set_yticklabels([f"{s}B" for s in sizes])
        ax.set_xlabel("Line Size")
        ax.set_ylabel("Cache Size")
        ax.set_title(f"Estimated Area (um²) — Assoc={assoc}")
        for yi in range(len(sizes)):
            for xi in range(len(lines)):
                ax.text(xi, yi, f"{area_grid[yi,xi]:.0f}",
                         ha='center', va='center', fontsize=8)
        plt.colorbar(im, ax=ax)
        plt.tight_layout()
        plt.savefig(os.path.join(VISOUT, f"sweep_a_area_a{assoc}.png"),
                    dpi=150)
        plt.close()
        print(f"  Saved sweep_a_area_a{assoc}.png")


# ============================================================
# Sweep (b): BP + Prefetcher — grouped bar chart
# ============================================================
def plot_sweep_b():
    sweep_dir = os.path.join(SIMOUT, "sweep_b")
    if not os.path.isdir(sweep_dir):
        print("sweep_b not found, skipping")
        return

    traces = ["coremark-10rnd-vld", "micro-train-vld"]
    bpus = ["none", "bimodal", "gshare", "tournament", "alwaystaken", "btfnt"]
    ipfs = ["none", "nextline", "stride", "tagged"]

    for trace in traces:
        short = "CoreMark" if "coremark" in trace else "MicroTrain"

        # IPC heatmap: BPU vs Prefetcher
        ipc_grid = np.zeros((len(bpus), len(ipfs)))
        for i, bpu in enumerate(bpus):
            for j, ipf in enumerate(ipfs):
                fname = f"{trace}_bp-{bpu}_pf-{ipf}.json"
                path = os.path.join(sweep_dir, fname)
                if os.path.isfile(path):
                    d = load_json(path)
                    if d:
                        ipc_grid[i, j] = get_ipc(d)

        fig, ax = plt.subplots(figsize=(8, 6))
        im = ax.imshow(ipc_grid, aspect='auto', cmap='YlGn')
        ax.set_xticks(range(len(ipfs)))
        ax.set_xticklabels(ipfs, rotation=45)
        ax.set_yticks(range(len(bpus)))
        ax.set_yticklabels(bpus)
        ax.set_xlabel("iCache Prefetcher")
        ax.set_ylabel("Branch Predictor")
        ax.set_title(f"{short} — IPC (512B/16B/1-way)")
        for yi in range(len(bpus)):
            for xi in range(len(ipfs)):
                v = ipc_grid[yi, xi]
                if v > 0:
                    ax.text(xi, yi, f"{v:.4f}",
                            ha='center', va='center', fontsize=8)
        plt.colorbar(im, ax=ax)
        plt.tight_layout()
        plt.savefig(os.path.join(VISOUT, f"sweep_b_{short}_ipc.png"), dpi=150)
        plt.close()
        print(f"  Saved sweep_b_{short}_ipc.png")

        # Stall breakdown for each BPU (no prefetcher)
        fig, ax = plt.subplots(figsize=(10, 6))
        x = np.arange(len(bpus))
        width = 0.15
        categories = ["Useful", "NoInst", "LsuStall", "BrMispred", "RAW"]
        colors = ["#2ecc71", "#3498db", "#e74c3c", "#f39c12", "#9b59b6"]
        bottom = np.zeros(len(bpus))
        for k, (cat, color) in enumerate(zip(categories, colors)):
            vals = []
            for bpu in bpus:
                fname = f"{trace}_bp-{bpu}_pf-none.json"
                path = os.path.join(sweep_dir, fname)
                if os.path.isfile(path):
                    d = load_json(path)
                    if d:
                        vals.append(get_stall_breakdown(d).get(cat, 0))
                    else:
                        vals.append(0)
                else:
                    vals.append(0)
            vals = np.array(vals)
            ax.bar(x, vals, bottom=bottom, label=cat, color=color)
            bottom += vals
        ax.set_xticks(x)
        ax.set_xticklabels(bpus, rotation=45)
        ax.set_ylabel("Fraction of Cycles")
        ax.set_title(f"{short} — Stall Breakdown by BP (no pf)")
        ax.legend(loc='upper right')
        plt.tight_layout()
        plt.savefig(os.path.join(VISOUT, f"sweep_b_{short}_stalls.png"),
                    dpi=150)
        plt.close()
        print(f"  Saved sweep_b_{short}_stalls.png")

    # Area comparison: BPU types (using coremark, no prefetcher)
    trace = "coremark-10rnd-vld"
    areas = []
    for bpu in bpus:
        fname = f"{trace}_bp-{bpu}_pf-none.json"
        ad = load_area("sweep_b", fname)
        areas.append(get_total_area(ad))
    if any(a > 0 for a in areas):
        fig, ax = plt.subplots(figsize=(8, 5))
        x = np.arange(len(bpus))
        ax.bar(x, areas, color='coral')
        ax.set_xticks(x)
        ax.set_xticklabels(bpus, rotation=45)
        ax.set_ylabel("Area (um²)")
        ax.set_title("Estimated Area by Branch Predictor")
        for i, v in enumerate(areas):
            if v > 0:
                ax.text(i, v + 100, f"{v:.0f}", ha='center',
                        va='bottom', fontsize=8)
        plt.tight_layout()
        plt.savefig(os.path.join(VISOUT, "sweep_b_area.png"), dpi=150)
        plt.close()
        print("  Saved sweep_b_area.png")


# ============================================================
# Sweep (c): dCache/StBuf sweep — grouped bar charts
# ============================================================
def plot_sweep_c():
    sweep_dir = os.path.join(SIMOUT, "sweep_c")
    if not os.path.isdir(sweep_dir):
        print("sweep_c not found, skipping")
        return

    traces = ["coremark-10rnd-vld", "micro-train-vld"]
    modes = ["ideal", "real"]
    stbuf_sizes = [2, 4, 8]
    dcache_sizes = [256, 512, 1024, 4096]
    dpfs = ["none", "stride"]

    for trace in traces:
        short = "CoreMark" if "coremark" in trace else "MicroTrain"

        for mode in modes:
            configs = []
            config_tags = []
            ipcs = []
            stall_data = []

            # StoreBuffer configs
            for stbsz in stbuf_sizes:
                tag = f"{trace}_{mode}_stbuf{stbsz}"
                path = os.path.join(sweep_dir, f"{tag}.json")
                if os.path.isfile(path):
                    d = load_json(path)
                    if d:
                        configs.append(f"StBuf{stbsz}")
                        config_tags.append(tag)
                        ipcs.append(get_ipc(d))
                        stall_data.append(get_stall_breakdown(d))

            # dCache configs
            for dsz in dcache_sizes:
                for dpf in dpfs:
                    tag = f"{trace}_{mode}_dc{dsz}_pf-{dpf}"
                    path = os.path.join(sweep_dir, f"{tag}.json")
                    if os.path.isfile(path):
                        d = load_json(path)
                        if d:
                            pf_label = f"+{dpf}" if dpf != "none" else ""
                            configs.append(f"dC{dsz}{pf_label}")
                            config_tags.append(tag)
                            ipcs.append(get_ipc(d))
                            stall_data.append(get_stall_breakdown(d))

            if not configs:
                continue

            # Load area data
            areas = []
            for tag_name in config_tags:
                ad = load_area("sweep_c", f"{tag_name}.json")
                areas.append(get_total_area(ad))

            fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(14, 14))

            x = np.arange(len(configs))
            ax1.bar(x, ipcs, color='steelblue')
            ax1.set_xticks(x)
            ax1.set_xticklabels(configs, rotation=45, ha='right')
            ax1.set_ylabel("IPC")
            ax1.set_title(f"{short} [{mode}] — IPC by Data-Side Config")
            for i, v in enumerate(ipcs):
                ax1.text(i, v + 0.001, f"{v:.4f}", ha='center',
                         va='bottom', fontsize=7)

            # Stacked stall breakdown
            categories = ["Useful", "NoInst", "LsuStall", "BrMispred", "RAW"]
            colors = ["#2ecc71", "#3498db", "#e74c3c", "#f39c12", "#9b59b6"]
            bottom = np.zeros(len(configs))
            for cat, color in zip(categories, colors):
                vals = np.array([sd.get(cat, 0) for sd in stall_data])
                ax2.bar(x, vals, bottom=bottom, label=cat, color=color)
                bottom += vals
            ax2.set_xticks(x)
            ax2.set_xticklabels(configs, rotation=45, ha='right')
            ax2.set_ylabel("Fraction of Cycles")
            ax2.set_title(f"{short} [{mode}] — Stall Breakdown")
            ax2.legend(loc='upper right')

            # Area bar chart
            ax3.bar(x, areas, color='coral')
            ax3.set_xticks(x)
            ax3.set_xticklabels(configs, rotation=45, ha='right')
            ax3.set_ylabel("Area (um²)")
            ax3.set_title(f"{short} [{mode}] — Estimated Area")
            for i, v in enumerate(areas):
                if v > 0:
                    ax3.text(i, v + 100, f"{v:.0f}", ha='center',
                             va='bottom', fontsize=7)

            plt.tight_layout()
            plt.savefig(os.path.join(VISOUT,
                                     f"sweep_c_{short}_{mode}.png"), dpi=150)
            plt.close()
            print(f"  Saved sweep_c_{short}_{mode}.png")


if __name__ == "__main__":
    print("=== Sweep (a): Cache Config ===")
    plot_sweep_a()
    print("=== Sweep (b): BP + Prefetcher ===")
    plot_sweep_b()
    print("=== Sweep (c): Data-Side Config ===")
    plot_sweep_c()
    print(f"\nAll plots saved to {VISOUT}/")
