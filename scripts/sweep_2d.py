#!/usr/bin/env python3
"""sweep_2d.py — 2D parameter sweep for npSim.

Runs the simulator over a Cartesian product of two parameter axes, estimates
chip area, and generates either heatmap or stacked-bar figures.

Usage:
  python3 sweep_2d.py --conf scripts/sweep_configs/cache_vs_line.py \\
                      --outdir my_cache_sweep [--fig-type heatmap] [--jobs 4]

  # With canonical naming (recommended — compatible with visual/plot_*.py):
  python3 sweep_2d.py --conf scripts/sweep_configs/npc_cal.py \\
                      --prefix coremark --outdir npc-cal --fig-type heatmap

  Canonical subdir format: {prefix}_l1i-{size}-b{blk}-a{assoc}[_l1d-...][_bpu-...]
  Compatible with: visual/plot_error_heatmap.py and visual/plot_perf.py

Config file (Python module defining these names):

  trace        = "tests/coremark-10rnd-vld.nptr.zst"

  axis1 = {
      "name": "cache_size",           # axis label — must differ from axis2
      "vals": [                        # one dict of npsim params per step
          {"l1i-size": "256B"},
          {"l1i-size": "512B"},
      ],
      "labels": ["256B", "512B"],      # optional; auto-derived from vals if absent
  }

  axis2 = {
      "name": "line_size",
      "vals": [{"l1i-blksize": "8"}, {"l1i-blksize": "16"}],
  }

  default_conf = {                     # npsim args applied to every run
      "l1i-assoc": "1",                # must NOT contain any key in axis1/axis2 vals
      "bpu-type": "bimodal",
  }

Figure layout:
  heatmap : IPC heatmap (top) + total area heatmap (bottom)
  stacked : IPC bars (top) + stall breakdown (middle) + area composition (bottom)
"""

import argparse
import importlib.util
import json
import os
import sys
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed
from itertools import product as cartesian_product
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

NPSIM_HOME = Path(os.environ.get("NPSIM_HOME", Path(__file__).resolve().parent.parent))
NPSIM_BIN  = NPSIM_HOME / "build" / "npsim.elf"
SIMOUT     = NPSIM_HOME / "simout"
AREAOUT    = NPSIM_HOME / "areaout"
AREA_EST   = NPSIM_HOME / "area" / "area_est.py"
PLOTS_ROOT = NPSIM_HOME / "visual" / "plots"


# ── Config loading ─────────────────────────────────────────────────────────────

def load_conf(path: str):
    """Import sweep config from a Python file via importlib."""
    spec = importlib.util.spec_from_file_location("_sweep_conf", path)
    mod  = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _axis_param_keys(axis):
    """Collect all npsim param keys declared in an axis's vals list."""
    keys = set()
    for v in axis["vals"]:
        if isinstance(v, dict):
            keys.update(k for k in v if k != "label")
    return keys


def validate_conf(axis1, axis2, default_conf):
    if axis1["name"] == axis2["name"]:
        raise ValueError("axis1.name and axis2.name must be different")
    all_axis_keys = _axis_param_keys(axis1) | _axis_param_keys(axis2)
    overlap = all_axis_keys & set(default_conf.keys())
    if overlap:
        raise ValueError(
            f"default_conf must not contain axis param keys: {sorted(overlap)}")


def _get_label(axis, idx):
    """Derive a human-readable label for axis value at index idx."""
    if "labels" in axis and idx < len(axis["labels"]):
        return str(axis["labels"][idx])
    v = axis["vals"][idx]
    if isinstance(v, dict):
        explicit = v.get("label")
        if explicit:
            return str(explicit)
        # Join all param values (skip the special "label" key)
        return "_".join(str(val) for k, val in v.items() if k != "label")
    return str(v)


def _get_params(axis_val):
    """Return {param: value} from one axis val entry."""
    if isinstance(axis_val, dict):
        return {k: v for k, v in axis_val.items() if k != "label"}
    return {}


def make_tag(outdir, axis1, axis2, i, j):
    """Unique simout subdirectory path for config (i, j)."""
    l1 = _get_label(axis1, i)
    l2 = _get_label(axis2, j)
    return f"{outdir}/{axis1['name']}-{l1}_{axis2['name']}-{l2}"


def _parse_size_str(s) -> int:
    """Parse size strings like '256B', '1kB', '4kB' to integer bytes."""
    if s is None:
        return 0
    s = str(s).strip()
    if s.lower().endswith("kb"):
        return int(s[:-2]) * 1024
    elif s.lower().endswith("mb"):
        return int(s[:-2]) * 1024 * 1024
    elif s.lower().endswith("b"):
        return int(s[:-1])
    else:
        try:
            return int(s)
        except ValueError:
            return 0


def encode_config(params: dict) -> str:
    """Encode npsim params dict to canonical config suffix string.

    Only encodes l1i/l1d/bpu components; ignores timing/mem params.
    Format: l1i-{size}-b{blk}-a{assoc}[_l1d-{size}-b{blk}-a{assoc}][_bpu-{type}-h{bht}-t{btb}]
    """
    parts = []
    l1i_s = _parse_size_str(params.get("l1i-size", "0"))
    if l1i_s > 0:
        blk   = int(params.get("l1i-blksize", 16))
        assoc = int(params.get("l1i-assoc", 1))
        parts.append(f"l1i-{l1i_s}-b{blk}-a{assoc}")
    l1d_s = _parse_size_str(params.get("l1d-size", "0"))
    if l1d_s > 0:
        blk   = int(params.get("l1d-blksize", 16))
        assoc = int(params.get("l1d-assoc", 1))
        parts.append(f"l1d-{l1d_s}-b{blk}-a{assoc}")
    bpu = params.get("bpu-type")
    if bpu and bpu != "none":
        bht = int(params.get("bpu-size", 0))
        btb = int(params.get("btb-size", 0))
        parts.append(f"bpu-{bpu}-h{bht}-t{btb}")
    return "_".join(parts) if parts else "default"


def make_canonical_tag(outdir, prefix, default_conf, v1, v2):
    """Build simout tag using canonical naming (with --prefix).

    Returns: '{outdir}/{prefix}_{config_suffix}'
    """
    merged = dict(default_conf)
    merged.update(_get_params(v1))
    merged.update(_get_params(v2))
    return f"{outdir}/{prefix}_{encode_config(merged)}"


# ── Multi-component config ─────────────────────────────────────────────────────

def _is_multicomp(conf) -> bool:
    """Return True if conf uses the new multi-component axis format."""
    return any(hasattr(conf, k)
               for k in ("icache_axis", "dcache_axis", "bpu_axis"))


def _expand_comp(comp: dict) -> list[dict]:
    """Expand a component axis dict to a flat list of merged param dicts."""
    axis1   = comp["axis1"]
    axis2   = comp.get("axis2")
    fixed   = comp.get("fixed", {})
    result  = []
    for v1 in axis1["vals"]:
        p1 = _get_params(v1)
        if axis2:
            for v2 in axis2["vals"]:
                result.append({**fixed, **p1, **_get_params(v2)})
        else:
            result.append({**fixed, **p1})
    return result


def build_multicomp_combos(conf, outdir: str, prefix: str | None
                           ) -> list[tuple[str, dict]]:
    """Build (tag, merged_params) list for a multi-component config.

    Each element is one simulation run.
    """
    default_conf = conf.default_conf
    comp_lists   = []
    for key in ("icache_axis", "dcache_axis", "bpu_axis"):
        if hasattr(conf, key):
            comp_lists.append(_expand_comp(getattr(conf, key)))
        else:
            comp_lists.append([{}])

    combos = []
    for parts in cartesian_product(*comp_lists):
        merged = dict(default_conf)
        for p in parts:
            merged.update(p)
        suffix = encode_config(merged)
        if prefix:
            tag = f"{outdir}/{prefix}_{suffix}"
        else:
            tag = f"{outdir}/{suffix}"
        combos.append((tag, merged))
    return combos


# ── Simulation ─────────────────────────────────────────────────────────────────

def _build_npsim_cmd(trace, outdir_tag, default_conf, v1, v2):
    """Build the full npsim command list for one (axis1, axis2) combination."""
    merged = dict(default_conf)
    merged.update(_get_params(v1))
    merged.update(_get_params(v2))
    cmd = [str(NPSIM_BIN), str(NPSIM_HOME / trace)]
    for k, v in merged.items():
        if v is None or v is True:
            cmd.append(f"--{k}")
        else:
            cmd += [f"--{k}", str(v)]
    cmd += ["--outdir", outdir_tag, "--print-none"]
    return cmd


def run_one_sim(trace, outdir_tag, default_conf, v1, v2):
    """Run npsim for one configuration. Returns (tag, ok, message)."""
    cmd = _build_npsim_cmd(trace, outdir_tag, default_conf, v1, v2)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=str(NPSIM_HOME))
        if r.returncode != 0:
            return outdir_tag, False, r.stderr.strip()
        return outdir_tag, True, ""
    except Exception as e:
        return outdir_tag, False, str(e)


def run_one_sim_merged(trace, outdir_tag, params):
    """Run npsim for one configuration given a pre-merged params dict."""
    cmd = [str(NPSIM_BIN), str(NPSIM_HOME / trace)]
    for k, v in params.items():
        if v is None or v is True:
            cmd.append(f"--{k}")
        else:
            cmd += [f"--{k}", str(v)]
    cmd += ["--outdir", outdir_tag, "--print-none"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=str(NPSIM_HOME))
        if r.returncode != 0:
            return outdir_tag, False, r.stderr.strip()
        return outdir_tag, True, ""
    except Exception as e:
        return outdir_tag, False, str(e)


def run_one_area(tag):
    """Run area_est.py for one configuration. Returns (tag, ok)."""
    conf_json = SIMOUT / tag / "conf.json"
    areaout   = AREAOUT / tag
    if not conf_json.exists():
        return tag, False
    try:
        r = subprocess.run(
            [sys.executable, str(AREA_EST),
             "--conf-json", str(conf_json),
             "--outdir",    str(areaout)],
            capture_output=True, text=True, cwd=str(NPSIM_HOME))
        return tag, r.returncode == 0
    except Exception:
        return tag, False


# ── Stats / area helpers ───────────────────────────────────────────────────────

def _load_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except Exception:
        return None


def load_stats(tag):
    return _load_json(SIMOUT / tag / "stats.json")


def load_area_comp(tag):
    return _load_json(AREAOUT / tag / "area_comp.json")


def get_ipc(data):
    if data is None:
        return 0.0
    s = data.get("stats1", data.get("stats0", {}))
    return s.get("Core", {}).get("ipc", 0.0)


def get_stall(data):
    if data is None:
        return {}
    s    = data.get("stats1", data.get("stats0", {}))
    core = s.get("Core", {})
    bd   = core.get("CycBreakdown", {})
    total = max(core.get("cycles", 1), 1)
    return {
        "Useful":    bd.get("NoStall", 0)         / total,
        "NoInst":    bd.get("NoInst", 0)          / total,
        "LsuStall":  bd.get("LsuStall", 0)        / total,
        "BrMispred": bd.get("BranchMispred", 0)   / total,
        "RAW":       bd.get("RAW", 0)             / total,
    }


def get_total_area(area_data):
    if area_data is None:
        return 0.0
    return area_data.get("total_area_um2", 0.0)


AREA_CATS   = ["Core", "iCache", "BPU", "BTB", "StBuf/dC", "Prefetcher", "SDRAM"]
AREA_COLORS = {
    "Core":       "#2c3e50",
    "iCache":     "#3498db",
    "BPU":        "#f39c12",
    "BTB":        "#e67e22",
    "StBuf/dC":   "#e74c3c",
    "Prefetcher": "#1abc9c",
    "SDRAM":      "#95a5a6",
}
COMP_TO_CAT = {
    "Core": "Core", "iCache": "iCache", "dCache": "StBuf/dC",
    "stBuf": "StBuf/dC", "BranchUnit": "BPU", "BimodalBP": "BPU",
    "GShareBP": "BPU", "TournamentBP": "BPU", "NoBPU": "BPU",
    "AlwaysTaken": "BPU", "BTFNTPredictor": "BPU",
    "BTB": "BTB", "NoBTB": "BTB", "SDRAM": "SDRAM",
    "iCache-NextLinePrefetcher": "Prefetcher",
    "iCache-StridePrefetcher":  "Prefetcher",
    "iCache-TaggedPrefetcher":  "Prefetcher",
}

STALL_CATS   = ["Useful", "NoInst", "LsuStall", "BrMispred", "RAW"]
STALL_COLORS = ["#2ecc71", "#3498db", "#e74c3c", "#f39c12", "#9b59b6"]

FALLBACK_NOTE_MODES = {
    "sram_analytical": "Analytical 6T SRAM cell model (CACTI unavailable)",
}


def get_area_breakdown(area_data):
    if area_data is None:
        return {}
    result = {c: 0.0 for c in AREA_CATS}
    for comp in area_data.get("components", []):
        cat = COMP_TO_CAT.get(comp["name"], "Core")
        result[cat] += comp["total_um2"]
    return result


def _sram_mode(v):
    if not isinstance(v, dict):
        return None
    mode = v.get("mode")
    if mode:
        return mode
    if v.get("dff_fallback"):
        return "dff_fallback"
    return None


def get_bar_fallbacks(area_data):
    if area_data is None:
        return []
    result = []
    for comp in area_data.get("components", []):
        for lbl, v in comp.get("sram_details", {}).items():
            mode = _sram_mode(v)
            if mode and mode in FALLBACK_NOTE_MODES:
                result.append((comp["name"], lbl, mode))
    return result


def _build_footnotes(all_fallbacks):
    """Return (registry, lines) from a list-of-lists of fallback tuples."""
    registry = {}
    lines = []
    for fb_list in all_fallbacks:
        for key in fb_list:
            if key not in registry:
                n = len(lines) + 1
                registry[key] = n
                comp, lbl, mode = key
                desc = FALLBACK_NOTE_MODES.get(mode, mode)
                lines.append(f"{n}. {comp}/{lbl}: {desc}")
    return registry, lines


def _add_footnotes(fig, lines):
    if not lines:
        return
    fig.text(0.01, 0.002, "\n".join(lines), fontsize=5.5,
             va='bottom', ha='left', family='monospace',
             bbox=dict(boxstyle='round,pad=0.3', facecolor='lightyellow',
                       alpha=0.7, edgecolor='#ccaa00'))


# ── Figures ────────────────────────────────────────────────────────────────────

def _shade_groups(ax, n1, n2):
    """Shade alternating axis1 groups with a light gray background."""
    for i in range(n1):
        if i % 2 == 1:
            ax.axvspan(i * n2 - 0.5, (i + 1) * n2 - 0.5, alpha=0.07, color='gray')


def plot_heatmap(axis1, axis2, tags_grid, title, outfig, figdir):
    """2-row figure: IPC heatmap (top) + total area heatmap (bottom)."""
    n1 = len(axis1["vals"])
    n2 = len(axis2["vals"])
    labels1 = [_get_label(axis1, i) for i in range(n1)]
    labels2 = [_get_label(axis2, j) for j in range(n2)]

    ipc_grid  = np.zeros((n1, n2))
    area_grid = np.zeros((n1, n2))
    cell_sups = [[[] for _ in range(n2)] for _ in range(n1)]
    footnote_reg  = {}
    footnote_lines = []

    for i in range(n1):
        for j in range(n2):
            tag = tags_grid[i][j]
            sd  = load_stats(tag)
            ad  = load_area_comp(tag)
            ipc_grid[i, j]  = get_ipc(sd)
            area_grid[i, j] = get_total_area(ad)
            fbs = get_bar_fallbacks(ad)
            sups = []
            for key in fbs:
                if key not in footnote_reg:
                    n = len(footnote_lines) + 1
                    footnote_reg[key] = n
                    comp, lbl, mode = key
                    footnote_lines.append(
                        f"{n}. {comp}/{lbl}: {FALLBACK_NOTE_MODES.get(mode, mode)}")
                sups.append(footnote_reg[key])
            cell_sups[i][j] = sorted(set(sups))

    figw = max(7, n2 * 1.6)
    figh = max(8, n1 * 1.4 * 2 + 2)
    fig, axes = plt.subplots(2, 1, figsize=(figw, figh))
    fig.suptitle(title, fontsize=11)

    for ax, grid, cmap, row_title in [
            (axes[0], ipc_grid,  'YlGn',  'IPC'),
            (axes[1], area_grid, 'OrRd',  'Total Area (um²)')]:
        im = ax.imshow(grid, aspect='auto', cmap=cmap)
        ax.set_xticks(range(n2)); ax.set_xticklabels(labels2, fontsize=8)
        ax.set_yticks(range(n1)); ax.set_yticklabels(labels1, fontsize=8)
        ax.set_xlabel(axis2["name"]); ax.set_ylabel(axis1["name"])
        has_note = row_title.startswith("Total") and footnote_lines
        ax.set_title(row_title + ("  [see footnotes]" if has_note else ""))
        plt.colorbar(im, ax=ax)
        is_area = row_title.startswith("Total")
        for yi in range(n1):
            for xi in range(n2):
                v = grid[yi, xi]
                extra = ""
                if is_area:
                    sups = cell_sups[yi][xi]
                    sup_str = ",".join(str(s) for s in sups)
                    extra = f"$^{{{sup_str}}}$" if sup_str else ""
                fmt = f"{v:.0f}" if is_area else f"{v:.4f}"
                ax.text(xi, yi, f"{fmt}{extra}",
                        ha='center', va='center', fontsize=8)

    plt.tight_layout()
    _add_footnotes(fig, footnote_lines)
    out = figdir / outfig
    plt.savefig(out, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved {out}")


def plot_stacked(axis1, axis2, tags_flat, labels_flat, title, outfig, figdir):
    """3-row figure: IPC bars (top) + stall breakdown (middle) + area (bottom).

    x-axis is the Cartesian product (axis1 × axis2) in row-major order.
    Alternating axis1 groups are shaded for readability.
    """
    n  = len(tags_flat)
    n1 = len(axis1["vals"])
    n2 = len(axis2["vals"])
    x  = np.arange(n)

    # Collect data
    ipcs   = [get_ipc(load_stats(t))   for t in tags_flat]
    stalls = [get_stall(load_stats(t)) for t in tags_flat]

    cat_vals = {c: [] for c in AREA_CATS}
    all_fallbacks = []
    area_totals   = []
    for t in tags_flat:
        ad = load_area_comp(t)
        bd = get_area_breakdown(ad)
        all_fallbacks.append(get_bar_fallbacks(ad))
        area_totals.append(get_total_area(ad))
        for c in AREA_CATS:
            cat_vals[c].append(bd.get(c, 0.0))

    registry, footnote_lines = _build_footnotes(all_fallbacks)

    figw = max(12, n * 0.9)
    fig, axes = plt.subplots(3, 1, figsize=(figw, 15))
    fig.suptitle(title, fontsize=11)

    # ── Row 1: IPC ──────────────────────────────────────────────────────────
    ax = axes[0]
    ax.bar(x, ipcs, color='#2ecc71', edgecolor='white')
    ax.set_xticks(x)
    ax.set_xticklabels(labels_flat, rotation=60, ha='right', fontsize=7)
    ax.set_ylabel("IPC")
    ax.set_title("IPC")
    _shade_groups(ax, n1, n2)

    # ── Row 2: Stall breakdown ───────────────────────────────────────────────
    ax = axes[1]
    bottom = np.zeros(n)
    for cat, color in zip(STALL_CATS, STALL_COLORS):
        vals = np.array([sd.get(cat, 0) for sd in stalls])
        ax.bar(x, vals, bottom=bottom, label=cat, color=color)
        bottom += vals
    ax.set_xticks(x)
    ax.set_xticklabels(labels_flat, rotation=60, ha='right', fontsize=7)
    ax.set_ylabel("Fraction of Cycles")
    ax.set_title("Cycle Breakdown")
    ax.legend(loc='upper right', fontsize=7)
    _shade_groups(ax, n1, n2)

    # ── Row 3: Area composition ─────────────────────────────────────────────
    ax = axes[2]
    bottom = np.zeros(n)
    for cat in AREA_CATS:
        vals = np.array(cat_vals[cat])
        if np.any(vals > 0):
            ax.bar(x, vals, bottom=bottom, label=cat,
                   color=AREA_COLORS.get(cat, "#7f8c8d"))
            bottom += vals
    ymax = float(bottom.max()) if len(bottom) > 0 and bottom.max() > 0 else 1.0
    for i, (tot, fb_list) in enumerate(zip(area_totals, all_fallbacks)):
        if tot > 0:
            sups    = sorted({registry[k] for k in fb_list if k in registry})
            sup_str = ",".join(str(s) for s in sups)
            label   = f"{tot:.0f}" + (f"$^{{{sup_str}}}$" if sup_str else "")
            ax.text(i, tot + ymax * 0.01, label,
                    ha='center', va='bottom', fontsize=6)
    ax.set_xticks(x)
    ax.set_xticklabels(labels_flat, rotation=60, ha='right', fontsize=7)
    ax.set_ylabel("Area (um²)")
    ax.set_title("Area Composition" + ("  [see footnotes]" if footnote_lines else ""))
    ax.legend(loc='upper left', fontsize=7)
    _shade_groups(ax, n1, n2)

    plt.tight_layout()
    _add_footnotes(fig, footnote_lines)
    out = figdir / outfig
    plt.savefig(out, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved {out}")


# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(
        description="2D parameter sweep for npSim",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Config file")[0])
    p.add_argument("--conf",  required=True,
                   help="Sweep config file (Python)")
    p.add_argument("--outdir", required=True,
                   help="Output name (written under simout/<outdir>/)")
    p.add_argument("--fig-type", choices=["heatmap", "stacked", "both"],
                   default="both",
                   help="Figure type (default: both)")
    p.add_argument("--jobs", type=int, default=4,
                   help="Parallel jobs for simulation and area estimation (default: 4)")
    p.add_argument("--no-sim",  action="store_true", help="Skip simulation")
    p.add_argument("--no-area", action="store_true", help="Skip area estimation")
    p.add_argument("--trace", default=None,
                   help="Override trace file from config")
    p.add_argument("--prefix", default=None,
                   help="Prefix for canonical subdir naming: "
                        "{outdir}/{prefix}_l1i-{size}-b{blk}-a{assoc}... "
                        "Required for compatibility with visual/plot_*.py")
    args = p.parse_args()

    # ── Load and validate config ───────────────────────────────────────────
    conf = load_conf(args.conf)
    default_conf = conf.default_conf
    trace        = args.trace or conf.trace
    outdir       = args.outdir

    # ── Branch: multi-component vs classic 2D sweep ───────────────────────
    if _is_multicomp(conf):
        combos = build_multicomp_combos(conf, outdir, args.prefix)
        total  = len(combos)
        comp_counts = []
        for key in ("icache_axis", "dcache_axis", "bpu_axis"):
            if hasattr(conf, key):
                comp = getattr(conf, key)
                n = len(comp["axis1"]["vals"]) * len(comp.get("axis2", {}).get("vals", [{}]))
                comp_counts.append(f"{key.replace('_axis','')}:{n}")
        print(f"Multi-component sweep '{outdir}': "
              f"{' × '.join(comp_counts)} = {total} configs")
        print(f"  trace = {trace}")

        if not args.no_sim:
            print(f"\n[1/2] Simulating  ({args.jobs} parallel jobs)…")
            with ThreadPoolExecutor(max_workers=args.jobs) as ex:
                futures = {
                    ex.submit(run_one_sim_merged, trace, tag, params): tag
                    for tag, params in combos
                }
                done = 0
                for fut in as_completed(futures):
                    tag, ok, msg = fut.result()
                    done += 1
                    short  = tag.split("/")[-1]
                    status = "ok" if ok else f"FAIL — {msg}"
                    print(f"  [{done:>{len(str(total))}}/{total}] {short}: {status}")
        else:
            print("[1/2] Simulation skipped (--no-sim)")

        if not args.no_area:
            print(f"\n[2/2] Estimating area  ({args.jobs} parallel jobs)…")
            with ThreadPoolExecutor(max_workers=args.jobs) as ex:
                futures = {ex.submit(run_one_area, tag): tag for tag, _ in combos}
                done = 0
                for fut in as_completed(futures):
                    tag, ok = fut.result()
                    done += 1
                    short = tag.split("/")[-1]
                    print(f"  [{done:>{len(str(total))}}/{total}] {short}: {'ok' if ok else 'FAIL'}")
        else:
            print("[2/2] Area estimation skipped (--no-area)")

        print("\nDone. Use visual/plot_perf.py or visual/plot_error_heatmap.py "
              "to visualise results.")
        return

    # ── Classic 2D sweep ──────────────────────────────────────────────────
    axis1 = conf.axis1
    axis2 = conf.axis2
    validate_conf(axis1, axis2, default_conf)

    n1 = len(axis1["vals"])
    n2 = len(axis2["vals"])

    print(f"Sweep '{outdir}': {n1}×{n2} = {n1*n2} configs")
    print(f"  axis1 = {axis1['name']}  ({n1} values)")
    print(f"  axis2 = {axis2['name']}  ({n2} values)")
    print(f"  trace = {trace}")

    # ── Build (tag, args) for each combo in row-major order ───────────────
    tags_grid  = [[None] * n2 for _ in range(n1)]
    tags_flat  = []
    labels_flat = []
    combos     = []   # (tag, v1, v2)
    for i in range(n1):
        for j in range(n2):
            if args.prefix:
                tag = make_canonical_tag(
                    outdir, args.prefix, default_conf,
                    axis1["vals"][i], axis2["vals"][j])
            else:
                tag = make_tag(outdir, axis1, axis2, i, j)
            tags_grid[i][j] = tag
            tags_flat.append(tag)
            labels_flat.append(
                f"{_get_label(axis1,i)}/{_get_label(axis2,j)}")
            combos.append((tag, axis1["vals"][i], axis2["vals"][j]))

    # ── Step 1: Run simulations ────────────────────────────────────────────
    if not args.no_sim:
        print(f"\n[1/3] Simulating  ({args.jobs} parallel jobs)…")
        with ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futures = {
                ex.submit(run_one_sim, trace, tag, default_conf, v1, v2): tag
                for tag, v1, v2 in combos
            }
            done = 0
            for fut in as_completed(futures):
                tag, ok, msg = fut.result()
                done += 1
                short = tag.split("/")[-1]
                status = "ok" if ok else f"FAIL — {msg}"
                print(f"  [{done:>{len(str(n1*n2))}}/{n1*n2}] {short}: {status}")
    else:
        print("[1/3] Simulation skipped (--no-sim)")

    # ── Step 2: Area estimation ────────────────────────────────────────────
    if not args.no_area:
        print(f"\n[2/3] Estimating area  ({args.jobs} parallel jobs)…")
        with ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futures = {ex.submit(run_one_area, tag): tag for tag, *_ in combos}
            done = 0
            for fut in as_completed(futures):
                tag, ok = fut.result()
                done += 1
                short = tag.split("/")[-1]
                print(f"  [{done:>{len(str(n1*n2))}}/{n1*n2}] {short}: {'ok' if ok else 'FAIL'}")
    else:
        print("[2/3] Area estimation skipped (--no-area)")

    # ── Step 3: Plot ───────────────────────────────────────────────────────
    figdir = PLOTS_ROOT / outdir
    os.makedirs(figdir, exist_ok=True)
    title  = f"{outdir}  |  {axis1['name']} × {axis2['name']}"
    safe   = outdir.replace("/", "_")
    print(f"\n[3/3] Plotting ({args.fig_type})…")
    if args.fig_type == "heatmap":
        plot_heatmap(axis1, axis2, tags_grid, title,
                     f"sweep2d_{safe}_heatmap.png", figdir)
    elif args.fig_type == "stacked":
        plot_stacked(axis1, axis2, tags_flat, labels_flat, title,
                     f"sweep2d_{safe}_stacked.png", figdir)
    else:
        plot_heatmap(axis1, axis2, tags_grid, title,
                     f"sweep2d_{safe}_heatmap.png", figdir)
        plot_stacked(axis1, axis2, tags_flat, labels_flat, title,
                     f"sweep2d_{safe}_stacked.png", figdir)


if __name__ == "__main__":
    main()
