#!/usr/bin/env python3
"""Visualize npSim sweep results from sweep_a, sweep_b, sweep_c.

Timing stats are plotted for CoreMark only.
Area is plotted for all configurations.
"""

import json
import os
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

SIMOUT = os.path.join(os.environ.get("NPSIM_HOME", "."), "simout")
AREAOUT = os.path.join(os.environ.get("NPSIM_HOME", "."), "areaout")
VISOUT = os.path.join(os.environ.get("NPSIM_HOME", "."), "visual", "plots")
os.makedirs(VISOUT, exist_ok=True)


def load_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (json.JSONDecodeError, ValueError, FileNotFoundError):
        return None


def load_stats(sweep, tag):
    for p in [os.path.join(SIMOUT, sweep, tag, "stats.json"),
              os.path.join(SIMOUT, sweep, f"{tag}.json")]:
        if os.path.isfile(p):
            return load_json(p)
    return None


def get_stats(data):
    return data.get("stats1", data.get("stats0", {}))


def get_ipc(data):
    return get_stats(data).get("Core", {}).get("ipc", 0)


def get_icache_mr(data):
    ic = get_stats(data).get("iCache", {})
    return ic.get("misses", 0) / max(ic.get("accesses", 1), 1)


def get_stall_breakdown(data):
    core = get_stats(data).get("Core", {})
    bd = core.get("CycBreakdown", core)
    total = max(core.get("cycles", 1), 1)
    return {
        "Useful": bd.get("NoStall", core.get("NoStall", 0)) / total,
        "NoInst": bd.get("NoInst", 0) / total,
        "LsuStall": bd.get("LsuStall", 0) / total,
        "BrMispred": bd.get("BranchMispred", 0) / total,
        "RAW": bd.get("RAW", 0) / total,
    }


def load_area(sweep, tag):
    for p in [os.path.join(AREAOUT, sweep, tag, "area_comp.json"),
              os.path.join(AREAOUT, sweep, tag.replace(".json", ""),
                           "area_comp.json")]:
        if os.path.isfile(p):
            return load_json(p)
    return None


def get_total_area(area_data):
    if area_data is None:
        return 0
    return area_data.get("total_area_um2", 0)


def has_dff_fallback(area_data):
    if area_data is None:
        return False
    for comp in area_data.get("components", []):
        for v in comp.get("sram_details", {}).values():
            if isinstance(v, dict) and v.get("dff_fallback"):
                return True
    return False


STALL_CATS = ["Useful", "NoInst", "LsuStall", "BrMispred", "RAW"]
STALL_COLORS = ["#2ecc71", "#3498db", "#e74c3c", "#f39c12", "#9b59b6"]

# Plot categories for area composition
AREA_CATS = ["Core", "iCache", "BPU", "BTB", "StBuf/dC", "Prefetcher",
             "SDRAM"]
AREA_CAT_COLORS = {
    "Core": "#2c3e50", "iCache": "#3498db", "BPU": "#f39c12",
    "BTB": "#e67e22", "StBuf/dC": "#e74c3c", "Prefetcher": "#1abc9c",
    "SDRAM": "#95a5a6",
}

# Map JSON component names → plot category
COMP_TO_CAT = {
    "Core": "Core",
    "iCache": "iCache", "dCache": "StBuf/dC", "stBuf": "StBuf/dC",
    "BranchUnit": "BPU", "BimodalBP": "BPU", "GShareBP": "BPU",
    "TournamentBP": "BPU", "NoBPU": "BPU", "AlwaysTaken": "BPU",
    "BTFNTPredictor": "BPU",
    "BTB": "BTB", "NoBTB": "BTB",
    "SDRAM": "SDRAM",
    "iCache-NextLinePrefetcher": "Prefetcher",
    "iCache-StridePrefetcher": "Prefetcher",
    "iCache-TaggedPrefetcher": "Prefetcher",
}


def get_area_breakdown(area_data):
    """Returns ({category: um²}, has_dff_fallback)."""
    if area_data is None:
        return {}, False
    result = {c: 0.0 for c in AREA_CATS}
    dff = False
    for comp in area_data.get("components", []):
        cat = COMP_TO_CAT.get(comp["name"], "Core")
        result[cat] += comp["total_um2"]
        for v in comp.get("sram_details", {}).values():
            if isinstance(v, dict) and v.get("dff_fallback"):
                dff = True
    return result, dff


def stacked_area_bar(ax, x, tags, sweep):
    """Draw stacked area composition bars. Returns has_dff list."""
    cat_vals = {c: [] for c in AREA_CATS}
    dff_list = []
    for tag in tags:
        bd, dff = get_area_breakdown(load_area(sweep, tag))
        dff_list.append(dff)
        for c in AREA_CATS:
            cat_vals[c].append(bd.get(c, 0))
    bottom = np.zeros(len(tags))
    for cat in AREA_CATS:
        vals = np.array(cat_vals[cat])
        if np.any(vals > 0):
            ax.bar(x, vals, bottom=bottom, label=cat,
                   color=AREA_CAT_COLORS.get(cat, "#7f8c8d"))
            bottom += vals
    ymax = max(bottom) if len(bottom) > 0 and max(bottom) > 0 else 1
    for i, (tot, dff) in enumerate(zip(bottom, dff_list)):
        if tot > 0:
            ax.text(i, tot + ymax * 0.01,
                    f"{tot:.0f}{'(*)' if dff else ''}",
                    ha='center', va='bottom', fontsize=6)
    return dff_list


def stacked_stall_bar(ax, x, stall_list):
    bottom = np.zeros(len(stall_list))
    for cat, color in zip(STALL_CATS, STALL_COLORS):
        vals = np.array([sd.get(cat, 0) for sd in stall_list])
        ax.bar(x, vals, bottom=bottom, label=cat, color=color)
        bottom += vals


def save(fname):
    plt.savefig(os.path.join(VISOUT, fname), dpi=150)
    plt.close()
    print(f"  Saved {fname}")


# Sweep (a): Cache config
def plot_sweep_a():
    sweep = "sweep_a"
    if not os.path.isdir(os.path.join(SIMOUT, sweep)):
        print("sweep_a not found, skipping"); return

    trace = "coremark-10rnd-vld"
    sizes = [256, 512, 1024, 4096]
    lines = [8, 16, 32, 64]
    assocs = [1, 2]

    for assoc in assocs:
        ipc_grid = np.zeros((len(sizes), len(lines)))
        mr_grid = np.zeros((len(sizes), len(lines)))
        for i, sz in enumerate(sizes):
            for j, ln in enumerate(lines):
                d = load_stats(sweep, f"{trace}_sz{sz}_ln{ln}_a{assoc}")
                if d:
                    ipc_grid[i, j] = get_ipc(d)
                    mr_grid[i, j] = get_icache_mr(d)

        fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))
        fig.suptitle(f"CoreMark — Assoc={assoc}")
        for ax, grid, cmap, title in [
                (ax1, ipc_grid, 'YlGn', 'IPC'),
                (ax2, mr_grid, 'YlOrRd', 'iCache Miss Rate')]:
            im = ax.imshow(grid, aspect='auto', cmap=cmap)
            ax.set_xticks(range(len(lines)))
            ax.set_xticklabels([f"{l}B" for l in lines])
            ax.set_yticks(range(len(sizes)))
            ax.set_yticklabels([f"{s}B" for s in sizes])
            ax.set_xlabel("Line Size"); ax.set_ylabel("Cache Size")
            ax.set_title(title)
            for yi in range(len(sizes)):
                for xi in range(len(lines)):
                    ax.text(xi, yi, f"{grid[yi, xi]:.4f}",
                            ha='center', va='center', fontsize=8)
            plt.colorbar(im, ax=ax)
        plt.tight_layout()
        save(f"sweep_a_CoreMark_a{assoc}.png")

    # Area heatmap with (*) for DFF cells
    for assoc in assocs:
        area_grid = np.zeros((len(sizes), len(lines)))
        dff_grid = np.full((len(sizes), len(lines)), False)
        for i, sz in enumerate(sizes):
            for j, ln in enumerate(lines):
                tag = f"{trace}_sz{sz}_ln{ln}_a{assoc}"
                ad = load_area(sweep, tag)
                area_grid[i, j] = get_total_area(ad)
                dff_grid[i, j] = has_dff_fallback(ad)

        fig, ax = plt.subplots(figsize=(7, 5))
        im = ax.imshow(area_grid, aspect='auto', cmap='OrRd')
        ax.set_xticks(range(len(lines)))
        ax.set_xticklabels([f"{l}B" for l in lines])
        ax.set_yticks(range(len(sizes)))
        ax.set_yticklabels([f"{s}B" for s in sizes])
        ax.set_xlabel("Line Size"); ax.set_ylabel("Cache Size")
        ax.set_title(f"Estimated Area (um²) — Assoc={assoc}  [(*) = DFF]")
        for yi in range(len(sizes)):
            for xi in range(len(lines)):
                v = area_grid[yi, xi]
                star = "(*)" if dff_grid[yi, xi] else ""
                ax.text(xi, yi, f"{v:.0f}{star}",
                        ha='center', va='center', fontsize=8)
        plt.colorbar(im, ax=ax)
        plt.tight_layout()
        save(f"sweep_a_area_a{assoc}.png")

    # Area composition + cycle breakdown
    for assoc in assocs:
        labels, tags = [], []
        for sz in sizes:
            for ln in lines:
                labels.append(f"{sz}B/{ln}B")
                tags.append(f"{trace}_sz{sz}_ln{ln}_a{assoc}")

        fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(16, 10))
        x = np.arange(len(labels))

        stacked_area_bar(ax1, x, tags, sweep)
        ax1.set_xticks(x)
        ax1.set_xticklabels(labels, rotation=60, ha='right', fontsize=7)
        ax1.set_ylabel("Area (um²)")
        ax1.set_title(
            f"Area Composition — Assoc={assoc}  [(*) = DFF estimate]")
        ax1.legend(loc='upper left', fontsize=7)

        stalls = []
        for t in tags:
            d = load_stats(sweep, t)
            stalls.append(get_stall_breakdown(d) if d else {})
        stacked_stall_bar(ax2, x, stalls)
        ax2.set_xticks(x)
        ax2.set_xticklabels(labels, rotation=60, ha='right', fontsize=7)
        ax2.set_ylabel("Fraction of Cycles")
        ax2.set_title(f"Cycle Breakdown (CoreMark) — Assoc={assoc}")
        ax2.legend(loc='upper right', fontsize=7)

        plt.tight_layout()
        save(f"sweep_a_composition_a{assoc}.png")


# Sweep (b): BP + Prefetcher
def plot_sweep_b():
    sweep = "sweep_b"
    if not os.path.isdir(os.path.join(SIMOUT, sweep)):
        print("sweep_b not found, skipping"); return

    trace = "coremark-10rnd-vld"
    bpus = ["none", "bimodal", "gshare", "tournament", "alwaystaken", "btfnt"]
    ipfs = ["none", "nextline", "stride", "tagged"]

    ipc_grid = np.zeros((len(bpus), len(ipfs)))
    for i, bpu in enumerate(bpus):
        for j, ipf in enumerate(ipfs):
            d = load_stats(sweep, f"{trace}_bp-{bpu}_pf-{ipf}")
            if d:
                ipc_grid[i, j] = get_ipc(d)

    fig, ax = plt.subplots(figsize=(8, 6))
    im = ax.imshow(ipc_grid, aspect='auto', cmap='YlGn')
    ax.set_xticks(range(len(ipfs))); ax.set_xticklabels(ipfs, rotation=45)
    ax.set_yticks(range(len(bpus))); ax.set_yticklabels(bpus)
    ax.set_xlabel("iCache Prefetcher"); ax.set_ylabel("Branch Predictor")
    ax.set_title("CoreMark — IPC (512B/16B/1-way)")
    for yi in range(len(bpus)):
        for xi in range(len(ipfs)):
            v = ipc_grid[yi, xi]
            if v > 0:
                ax.text(xi, yi, f"{v:.4f}",
                        ha='center', va='center', fontsize=8)
    plt.colorbar(im, ax=ax); plt.tight_layout()
    save("sweep_b_CoreMark_ipc.png")

    fig, ax = plt.subplots(figsize=(10, 6))
    x = np.arange(len(bpus))
    stalls = []
    for bpu in bpus:
        d = load_stats(sweep, f"{trace}_bp-{bpu}_pf-none")
        stalls.append(get_stall_breakdown(d) if d else {})
    stacked_stall_bar(ax, x, stalls)
    ax.set_xticks(x); ax.set_xticklabels(bpus, rotation=45)
    ax.set_ylabel("Fraction of Cycles")
    ax.set_title("CoreMark — Stall Breakdown by BP (no pf)")
    ax.legend(loc='upper right')
    plt.tight_layout()
    save("sweep_b_CoreMark_stalls.png")

    fig, ax = plt.subplots(figsize=(10, 6))
    x = np.arange(len(bpus))
    tags = [f"{trace}_bp-{b}_pf-none" for b in bpus]
    stacked_area_bar(ax, x, tags, sweep)
    ax.set_xticks(x); ax.set_xticklabels(bpus, rotation=45)
    ax.set_ylabel("Area (um²)")
    ax.set_title("Area Composition by BP  [(*) = DFF estimate]")
    ax.legend(loc='upper left', fontsize=7)
    plt.tight_layout()
    save("sweep_b_area.png")


# Sweep (c): dCache/StBuf
def plot_sweep_c():
    sweep = "sweep_c"
    if not os.path.isdir(os.path.join(SIMOUT, sweep)):
        print("sweep_c not found, skipping"); return

    trace = "coremark-10rnd-vld"
    modes = ["ideal", "real"]
    stbuf_sizes = [2, 4, 8]
    dcache_sizes = [256, 512, 1024, 4096]
    dpfs = ["none", "stride"]

    for mode in modes:
        configs, config_tags, ipcs, stall_data = [], [], [], []

        for stbsz in stbuf_sizes:
            tag = f"{trace}_{mode}_stbuf{stbsz}"
            d = load_stats(sweep, tag)
            if d:
                configs.append(f"StBuf{stbsz}")
                config_tags.append(tag)
                ipcs.append(get_ipc(d))
                stall_data.append(get_stall_breakdown(d))

        for dsz in dcache_sizes:
            for dpf in dpfs:
                tag = f"{trace}_{mode}_dc{dsz}_pf-{dpf}"
                d = load_stats(sweep, tag)
                if d:
                    pf = f"+{dpf}" if dpf != "none" else ""
                    configs.append(f"dC{dsz}{pf}")
                    config_tags.append(tag)
                    ipcs.append(get_ipc(d))
                    stall_data.append(get_stall_breakdown(d))

        if not configs:
            continue

        fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(14, 14))
        x = np.arange(len(configs))

        ax1.bar(x, ipcs, color='steelblue')
        ax1.set_xticks(x)
        ax1.set_xticklabels(configs, rotation=45, ha='right')
        ax1.set_ylabel("IPC")
        ax1.set_title(f"CoreMark [{mode}] — IPC")
        for i, v in enumerate(ipcs):
            ax1.text(i, v + 0.001, f"{v:.4f}", ha='center',
                     va='bottom', fontsize=7)

        stacked_stall_bar(ax2, x, stall_data)
        ax2.set_xticks(x)
        ax2.set_xticklabels(configs, rotation=45, ha='right')
        ax2.set_ylabel("Fraction of Cycles")
        ax2.set_title(f"CoreMark [{mode}] — Stall Breakdown")
        ax2.legend(loc='upper right')

        stacked_area_bar(ax3, x, config_tags, sweep)
        ax3.set_xticks(x)
        ax3.set_xticklabels(configs, rotation=45, ha='right')
        ax3.set_ylabel("Area (um²)")
        ax3.set_title(
            f"CoreMark [{mode}] — Area Composition  [(*) = DFF]")
        ax3.legend(loc='upper left', fontsize=7)

        plt.tight_layout()
        save(f"sweep_c_CoreMark_{mode}.png")


if __name__ == "__main__":
    print("=== Sweep (a): Cache Config ===")
    plot_sweep_a()
    print("=== Sweep (b): BP + Prefetcher ===")
    plot_sweep_b()
    print("=== Sweep (c): Data-Side Config ===")
    plot_sweep_c()
    print(f"\nAll plots saved to {VISOUT}/")
