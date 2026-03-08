#!/usr/bin/env python3
"""plot_perf.py — npSim performance heatmap (IPC / miss rates).

Each enabled component (--icache / --dcache / --bpu) receives a sweep config
.py file. Configs may use either:

  • Old 2-axis format: axis1, axis2, default_conf  (e.g. npc_cal.py)
  • New multi-component format: icache_axis, dcache_axis, bpu_axis, default_conf
    (e.g. cfg_ext_valid.py)

Layout: one subplot per enabled component, arranged horizontally.
Non-active components are fixed at their first configured value.

Metrics (--metric):
  ipc           — Instructions Per Cycle
  l1i_missrate  — L1 iCache miss rate
  l1d_missrate  — L1 dCache miss rate
  bpu_missrate  — Branch predictor miss rate

Usage (old format, single component):
  python3 visual/plot_perf.py \\
      --sim-dir simout/npc-cal \\
      --prefix  coremark --metric ipc \\
      --icache  scripts/sweep_configs/npc_cal.py \\
      --outfile visual/plots/npc-cal/perf_ipc.png

Usage (new multi-component format, --sweep-file shorthand):
  python3 visual/plot_perf.py \\
      --sim-dir simout/cfg-ext-valid \\
      --prefix  cfg-ext-valid --metric ipc \\
      --sweep-file scripts/sweep_configs/cfg_ext_valid.py \\
      --outfile visual/plots/cfg-ext-valid/perf_ipc.png

Subdir naming:
  Canonical: {prefix}_l1i-{sz}-b{blk}-a{assoc}[_l1d-...][_bpu-...]
  Legacy:    cache_size-{sz}B_line_size-{blk}B
"""

import argparse
import importlib.util
import json
import os
import sys
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

NPSIM_HOME = Path(os.environ.get("NPSIM_HOME",
                                  Path(__file__).resolve().parent.parent))

METRIC_LABEL = {
    "ipc":          "IPC",
    "l1i_missrate": "L1I Miss Rate",
    "l1d_missrate": "L1D Miss Rate",
    "bpu_missrate": "BPU Miss Rate",
}
METRIC_CMAP = {
    "ipc":          "RdYlBu",
    "l1i_missrate": "YlOrRd",
    "l1d_missrate": "YlOrRd",
    "bpu_missrate": "YlOrRd",
}
METRIC_FMT = {
    "ipc":          ".4f",
    "l1i_missrate": ".2%",
    "l1d_missrate": ".2%",
    "bpu_missrate": ".2%",
}


# ── Sweep config loader ────────────────────────────────────────────────────────

def load_conf(path: str):
    p = Path(path)
    if not p.is_absolute():
        p = NPSIM_HOME / p
    spec = importlib.util.spec_from_file_location("_conf", p)
    mod  = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def get_label(axis, idx: int) -> str:
    if "labels" in axis and idx < len(axis["labels"]):
        return str(axis["labels"][idx])
    v = axis["vals"][idx]
    if isinstance(v, dict):
        lbl = v.get("label")
        if lbl:
            return str(lbl)
        return "_".join(str(val) for k, val in v.items() if k != "label")
    return str(v)


def get_params(val) -> dict:
    if isinstance(val, dict):
        return {k: v for k, v in val.items() if k != "label"}
    return {}


# ── Size utilities ─────────────────────────────────────────────────────────────

def parse_size(s) -> int:
    s = str(s).strip()
    if s.lower().endswith("kb"):
        return int(s[:-2]) * 1024
    elif s.lower().endswith("mb"):
        return int(s[:-2]) * 1024 * 1024
    elif s.lower().endswith("b"):
        return int(s[:-1])
    return int(s)


def size_label(n: int) -> str:
    if n >= 1024 and n % 1024 == 0:
        return f"{n // 1024}kB"
    return f"{n}B"


# ── Config extraction ──────────────────────────────────────────────────────────

def extract_params(merged: dict) -> dict:
    l1i_sz  = parse_size(merged.get("l1i-size", "0"))
    l1i_blk = parse_size(merged.get("l1i-blksize", "16"))
    l1i_a   = int(merged.get("l1i-assoc", 1))
    l1d_sz  = parse_size(merged.get("l1d-size", "0"))
    l1d_blk = parse_size(merged.get("l1d-blksize", "16"))
    l1d_a   = int(merged.get("l1d-assoc", 1))
    bpu_t   = str(merged.get("bpu-type", "none") or "none")
    bpu_bht = int(merged.get("bpu-size", merged.get("bht-size", 0)) or 0)
    bpu_btb = int(merged.get("btb-size", 0) or 0)
    return dict(l1i_sz=l1i_sz, l1i_blk=l1i_blk, l1i_a=l1i_a,
                l1d_sz=l1d_sz, l1d_blk=l1d_blk, l1d_a=l1d_a,
                bpu_t=bpu_t,   bpu_bht=bpu_bht,  bpu_btb=bpu_btb)


# ── Canonical naming ───────────────────────────────────────────────────────────

def canonical_name(prefix: str, p: dict) -> str:
    parts = []
    if p["l1i_sz"] > 0:
        parts.append(f"l1i-{p['l1i_sz']}-b{p['l1i_blk']}-a{p['l1i_a']}")
    if p["l1d_sz"] > 0:
        parts.append(f"l1d-{p['l1d_sz']}-b{p['l1d_blk']}-a{p['l1d_a']}")
    if p["bpu_t"] and p["bpu_t"] != "none":
        parts.append(f"bpu-{p['bpu_t']}-h{p['bpu_bht']}-t{p['bpu_btb']}")
    cfg = "_".join(parts) if parts else "default"
    return f"{prefix}_{cfg}"


# ── Stats loading ──────────────────────────────────────────────────────────────

def _load_json(path: Path) -> dict | None:
    if not path.exists():
        return None
    try:
        with open(path) as f:
            return json.load(f)
    except Exception:
        return None


def find_sim_stats(sim_dir: Path, prefix: str, p: dict) -> dict | None:
    d = _load_json(sim_dir / canonical_name(prefix, p) / "stats.json")
    if d is not None:
        return d
    if p["l1i_sz"] > 0:
        leg = (f"cache_size-{size_label(p['l1i_sz'])}"
               f"_line_size-{p['l1i_blk']}B")
        d = _load_json(sim_dir / leg / "stats.json")
        if d is not None:
            return d
    if p["bpu_t"] != "none" and p["l1i_sz"] == 0:
        leg2 = f"{p['bpu_t']}_btb{p['bpu_btb']}_ras0"
        d = _load_json(sim_dir / leg2 / "stats.json")
        if d is not None:
            return d
    return None


def get_metric(data: dict, metric: str) -> float:
    if data is None:
        return float('nan')
    s = data.get("stats1", data.get("stats0", {}))
    if metric == "ipc":
        return s.get("Core", {}).get("ipc", float('nan'))
    if metric == "l1i_missrate":
        ic    = s.get("iCache", {})
        hits  = ic.get("hits", 0)
        miss  = ic.get("misses", 0)
        total = hits + miss
        return miss / total if total > 0 else float('nan')
    if metric == "l1d_missrate":
        dc   = s.get("dCache", {})
        acc  = dc.get("accesses", 0)
        hits = dc.get("hits", 0)
        return (acc - hits) / acc if acc > 0 else float('nan')
    if metric == "bpu_missrate":
        return s.get("BranchUnit", {}).get("miss_rate", float('nan'))
    return float('nan')


# ── Panel computation ──────────────────────────────────────────────────────────

_DUMMY_AXIS2 = {"name": "", "vals": [{}], "labels": [""]}


def _get_comp_axes(conf, component: str):
    """Extract (axis1, axis2, comp_fixed) for a component from a config module.

    Handles both old format (axis1/axis2/default_conf) and new multi-component
    format (icache_axis/dcache_axis/bpu_axis with optional axis2 and fixed).
    Returns (axis1, axis2, comp_fixed) or (None, None, None) if not present.
    """
    comp_key = f"{component}_axis"
    if hasattr(conf, comp_key):
        comp  = getattr(conf, comp_key)
        axis1 = comp["axis1"]
        axis2 = comp.get("axis2", _DUMMY_AXIS2)
        fixed = comp.get("fixed", {})
        return axis1, axis2, fixed
    if component == "icache" and hasattr(conf, "axis1"):
        return conf.axis1, conf.axis2, {}
    return None, None, None


def get_first_val_params(conf, component: str) -> dict:
    """Return merged params for the first configuration of a component.

    Used to fix non-active components at a baseline when plotting a panel.
    """
    axis1, axis2, fixed = _get_comp_axes(conf, component)
    if axis1 is None:
        return {}
    params = dict(fixed)
    params.update(get_params(axis1["vals"][0]))
    if axis2 and axis2["vals"]:
        params.update(get_params(axis2["vals"][0]))
    return params


def compute_panel(conf, component: str, context_params: dict,
                  sim_dir: Path, prefix: str, metric: str):
    """Compute metric grid for one component panel.

    conf:           loaded config module
    component:      "icache" | "dcache" | "bpu"
    context_params: pre-merged fixed params for non-active components
    """
    axis1, axis2, comp_fixed = _get_comp_axes(conf, component)
    if axis1 is None:
        return None

    default_conf = conf.default_conf
    n1, n2 = len(axis1["vals"]), len(axis2["vals"])

    labels1 = [get_label(axis1, i) for i in range(n1)]
    labels2 = [get_label(axis2, j) for j in range(n2)]
    grid    = np.full((n1, n2), float('nan'))

    missing = 0
    for i, v1 in enumerate(axis1["vals"]):
        for j, v2 in enumerate(axis2["vals"]):
            merged = dict(default_conf)
            merged.update(context_params)
            merged.update(comp_fixed)
            merged.update(get_params(v1))
            merged.update(get_params(v2))
            p    = extract_params(merged)
            data = find_sim_stats(sim_dir, prefix, p)
            if data is None:
                missing += 1
            grid[i, j] = get_metric(data, metric)

    if missing:
        print(f"  [warn] {missing}/{n1*n2} sim results not found",
              file=sys.stderr)

    return dict(grid=grid, labels1=labels1, labels2=labels2,
                name1=axis1["name"], name2=axis2["name"])


# ── Subplot drawing ────────────────────────────────────────────────────────────

def draw_metric_ax(ax, result: dict, metric: str, title: str):
    grid    = result["grid"]
    labels1 = result["labels1"]
    labels2 = result["labels2"]
    cmap    = METRIC_CMAP.get(metric, "viridis")
    fmt     = METRIC_FMT.get(metric, ".4f")
    label   = METRIC_LABEL.get(metric, metric)

    if np.all(np.isnan(grid)):
        ax.text(0.5, 0.5, "No data", ha='center', va='center',
                transform=ax.transAxes, color='gray', fontsize=11)
        ax.set_title(title, fontsize=10)
        return

    im = ax.imshow(grid, cmap=cmap, aspect='auto')
    ax.set_xticks(range(len(labels2)))
    ax.set_xticklabels(labels2, fontsize=8)
    ax.set_yticks(range(len(labels1)))
    ax.set_yticklabels(labels1, fontsize=8)
    ax.set_xlabel(result["name2"], fontsize=9)
    ax.set_ylabel(result["name1"], fontsize=9)
    ax.set_title(title, fontsize=10)
    plt.colorbar(im, ax=ax, shrink=0.85, label=label)

    vmin, vmax_ = float(np.nanmin(grid)), float(np.nanmax(grid))
    for yi in range(len(labels1)):
        for xi in range(len(labels2)):
            v = grid[yi, xi]
            if not np.isnan(v):
                brightness = (v - vmin) / max(vmax_ - vmin, 1e-9)
                color = 'white' if brightness < 0.5 else 'black'
                ax.text(xi, yi, format(v, fmt),
                        ha='center', va='center', fontsize=8, color=color)


# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(
        description="npSim performance heatmap",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    p.add_argument("--sim-dir",  required=True,
                   help="npSim simout sweep dir (simout/<name>)")
    p.add_argument("--prefix",   required=True,
                   help="Subdir prefix used during sweep (e.g. 'coremark')")
    p.add_argument("--metric",   required=True,
                   choices=list(METRIC_LABEL.keys()),
                   help="Metric to display")
    p.add_argument("--outfile",  default=None,
                   help="Output PNG (default: visual/plots/<sim-dir>/perf_{metric}.png)")
    p.add_argument("--title",    default="",
                   help="Overall figure title prefix")
    p.add_argument("--icache",   metavar="CONF.py",
                   help="iCache panel: sweep config .py file")
    p.add_argument("--dcache",   metavar="CONF.py",
                   help="dCache panel: sweep config .py file")
    p.add_argument("--bpu",      metavar="CONF.py",
                   help="BPU panel: sweep config .py file")
    p.add_argument("--sweep-file", metavar="CONF.py",
                   help="Use one config .py for all components "
                        "(auto-detects icache/dcache/bpu axes)")
    args = p.parse_args()

    if args.sweep_file:
        sf = args.sweep_file
        conf = load_conf(sf)
        enabled = []
        for comp in ("icache", "dcache", "bpu"):
            if hasattr(conf, f"{comp}_axis"):
                enabled.append((comp, sf))
            elif comp == "icache" and hasattr(conf, "axis1"):
                enabled.append((comp, sf))
    else:
        enabled = [(c, getattr(args, c))
                   for c in ("icache", "dcache", "bpu")
                   if getattr(args, c) is not None]
    if not enabled:
        print("ERROR: specify at least one of --icache / --dcache / --bpu "
              "or --sweep-file CONF.py", file=sys.stderr)
        sys.exit(1)

    sim_dir = (NPSIM_HOME / args.sim_dir
               if not Path(args.sim_dir).is_absolute()
               else Path(args.sim_dir))

    if args.outfile:
        outfile = (NPSIM_HOME / args.outfile
                   if not Path(args.outfile).is_absolute()
                   else Path(args.outfile))
    else:
        tag = Path(args.sim_dir).name
        outfile = (NPSIM_HOME / "visual" / "plots" / tag
                   / f"perf_{args.metric}.png")
    outfile.parent.mkdir(parents=True, exist_ok=True)

    n_panels   = len(enabled)
    metric_lbl = METRIC_LABEL.get(args.metric, args.metric)
    title_base = (args.title
                  or f"npSim {metric_lbl}: {args.prefix} / {sim_dir.name}")

    # Load all confs upfront so we can build cross-component context
    loaded = {comp: load_conf(path) for comp, path in enabled}

    fig, axes = plt.subplots(1, n_panels,
                             figsize=(7 * n_panels, 6),
                             squeeze=False)
    fig.suptitle(title_base, fontsize=13, fontweight='bold')

    for col, (comp_name, conf_path) in enumerate(enabled):
        conf = loaded[comp_name]
        print(f"[{comp_name}] loading conf: {conf_path}")

        # Fix other components at their first configured value
        context = {}
        for other_comp, other_conf in loaded.items():
            if other_comp != comp_name:
                context.update(get_first_val_params(other_conf, other_comp))

        result = compute_panel(conf, comp_name, context,
                               sim_dir, args.prefix, args.metric)
        if result is None:
            print(f"  [warn] no {comp_name} axis found in {conf_path}",
                  file=sys.stderr)
            continue
        draw_metric_ax(axes[0, col], result, args.metric,
                       f"{comp_name.upper()} {metric_lbl}")

    plt.tight_layout(rect=[0, 0, 1, 0.94])
    plt.savefig(outfile, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved {outfile}")


if __name__ == "__main__":
    main()
