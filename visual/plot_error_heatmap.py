#!/usr/bin/env python3
"""plot_error_heatmap.py — npSim vs RTL IPC error heatmap.

Each enabled component (--icache / --dcache / --bpu) receives a sweep config
.py file. Configs may use either:

  • Old 2-axis format: axis1, axis2, default_conf  (e.g. soc_cal.py)
  • New multi-component format: icache_axis, dcache_axis, bpu_axis, default_conf
    (e.g. cfg_ext_valid.py)

Layout: one row of 3 subplots per enabled component (error | RTL IPC | Sim IPC).
Non-active components are fixed at their first configured value.

Usage (old format, iCache only, NPC mode):
  python3 visual/plot_error_heatmap.py \\
      --sim-dir  simout/npc-cal \\
      --rtl-dir  $NPC_HOME/ccout/sweep-cache-coremark \\
      --prefix   coremark --npc-mode \\
      --icache   scripts/sweep_configs/npc_cal.py \\
      --outfile  visual/plots/npc-cal/error_heatmap.png

Usage (new multi-component format, --sweep-file shorthand):
  python3 visual/plot_error_heatmap.py \\
      --sim-dir  simout/cfg-ext-valid \\
      --rtl-dir  $NPC_HOME/ccout/sweep-cache-coremark-soc \\
      --prefix   cfg-ext-valid \\
      --sweep-file scripts/sweep_configs/cfg_ext_valid.py \\
      --outfile  visual/plots/cfg-ext-valid/error_heatmap.png

Subdir naming (sim and RTL):
  Canonical: {prefix}_l1i-{sz}-b{blk}-a{assoc}[_l1d-...][_bpu-...]
  Legacy sim: cache_size-{sz}B_line_size-{blk}B   (sweep_2d.py without --prefix)
  Legacy RTL: l1i_{sz}_blk{blk}_assoc{assoc}       (pre-rename)
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
from matplotlib.colors import TwoSlopeNorm

NPSIM_HOME = Path(os.environ.get("NPSIM_HOME",
                                  Path(__file__).resolve().parent.parent))


# ── Sweep config loader ────────────────────────────────────────────────────────

def load_conf(path: str):
    """Load sweep config .py (same format as scripts/sweep_configs/*.py)."""
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
    """'256B' / '1kB' / '16' / 16 → bytes."""
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
    """Extract l1i/l1d/bpu canonical params from a merged npsim param dict."""
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

def canonical_name(prefix, p: dict) -> str:
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
    """Try canonical name first, then legacy sweep_2d.py format."""
    # Canonical
    d = _load_json(sim_dir / canonical_name(prefix, p) / "stats.json")
    if d is not None:
        return d
    # Legacy sweep_2d.py: cache_size-{sz}B_line_size-{blk}B
    if p["l1i_sz"] > 0:
        leg = (f"cache_size-{size_label(p['l1i_sz'])}"
               f"_line_size-{p['l1i_blk']}B")
        d = _load_json(sim_dir / leg / "stats.json")
        if d is not None:
            return d
    # Legacy BPU sweep: {type}_btb{btb}_ras0
    if p["bpu_t"] != "none" and p["l1i_sz"] == 0:
        leg2 = f"{p['bpu_t']}_btb{p['bpu_btb']}_ras0"
        d = _load_json(sim_dir / leg2 / "stats.json")
        if d is not None:
            return d
    return None


def find_rtl_stats(rtl_dir: Path, prefix: str, p: dict) -> dict | None:
    """Try canonical RTL name first, then legacy l1i_{sz}_blk{blk}_assoc{a}."""
    d = _load_json(rtl_dir / canonical_name(prefix, p) / "stats.json")
    if d is not None:
        return d
    # Legacy RTL naming
    if p["l1i_sz"] > 0:
        leg = f"l1i_{p['l1i_sz']}_blk{p['l1i_blk']}_assoc{p['l1i_a']}"
        d = _load_json(rtl_dir / leg / "stats.json")
        if d is not None:
            return d
    return None


def get_sim_ipc(data: dict) -> float:
    if data is None:
        return float('nan')
    s = data.get("stats1", data.get("stats0", {}))
    return s.get("Core", {}).get("ipc", float('nan'))


def get_rtl_ipc(data: dict) -> float:
    if data is None:
        return float('nan')
    return data.get("pmu", {}).get("ipc", float('nan'))


def get_sim_stall(data: dict) -> dict:
    if data is None:
        return {}
    s = data.get("stats1", data.get("stats0", {}))
    bd    = s.get("Core", {}).get("CycBreakdown", {})
    total = max(s.get("Core", {}).get("cycles", 1), 1)
    return {k: bd.get(k, 0) / total * 100 for k in
            ("NoStall", "NoInst", "LsuStall", "BranchMispred", "RAW")}


def get_rtl_stall(data: dict) -> dict:
    if data is None:
        return {}
    bc    = data.get("pmu", {}).get("BlockedCause", {})
    total = max(bc.get("samples", 1), 1)
    return {
        "NoStall":       bc.get("NoStall",        0) / total * 100,
        "NoInst":        bc.get("NoInst",          0) / total * 100,
        "LsuStall":      bc.get("LsuStall",        0) / total * 100,
        "BranchMispred": bc.get("BranchMispred",   0) / total * 100,
        "RAW":           bc.get("RAW",             0) / total * 100,
    }


# ── Panel computation ──────────────────────────────────────────────────────────

_DUMMY_AXIS2 = {"name": "", "vals": [{}], "labels": [""]}


def _get_comp_axes(conf, component: str):
    """Extract (axis1, axis2, comp_fixed) for a component.

    Handles old format (axis1/axis2) and new multi-component format
    (icache_axis/dcache_axis/bpu_axis). Returns (None,None,None) if absent.
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
    """Return merged params for the first configuration of a component."""
    axis1, axis2, fixed = _get_comp_axes(conf, component)
    if axis1 is None:
        return {}
    params = dict(fixed)
    params.update(get_params(axis1["vals"][0]))
    if axis2 and axis2["vals"]:
        params.update(get_params(axis2["vals"][0]))
    return params


def compute_panel(conf, component: str, context_params: dict,
                  sim_dir: Path, rtl_dir: Path | None, prefix: str):
    """Compute ipc_err / rtl_ipc / sim_ipc grids for one component panel.

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

    ipc_err  = np.full((n1, n2), np.nan)
    rtl_ipc  = np.full((n1, n2), np.nan)
    sim_ipc  = np.full((n1, n2), np.nan)
    stall_cats = ["NoInst", "LsuStall", "BranchMispred"]
    stall_err  = {c: np.full((n1, n2), np.nan) for c in stall_cats}

    missing_sim = 0
    missing_rtl = 0

    for i, v1 in enumerate(axis1["vals"]):
        for j, v2 in enumerate(axis2["vals"]):
            merged = dict(default_conf)
            merged.update(context_params)
            merged.update(comp_fixed)
            merged.update(get_params(v1))
            merged.update(get_params(v2))
            p = extract_params(merged)

            sim_data = find_sim_stats(sim_dir, prefix, p)
            rtl_data = (find_rtl_stats(rtl_dir, prefix, p)
                        if rtl_dir is not None else None)

            if sim_data is None:
                missing_sim += 1
            if rtl_data is None and rtl_dir is not None:
                missing_rtl += 1

            s_ipc = get_sim_ipc(sim_data)
            r_ipc = get_rtl_ipc(rtl_data)
            sim_ipc[i, j] = s_ipc
            rtl_ipc[i, j] = r_ipc

            if not np.isnan(r_ipc) and r_ipc > 0 and not np.isnan(s_ipc):
                ipc_err[i, j] = (s_ipc - r_ipc) / r_ipc * 100

            if sim_data is not None and rtl_data is not None:
                ss = get_sim_stall(sim_data)
                rs = get_rtl_stall(rtl_data)
                for c in stall_cats:
                    stall_err[c][i, j] = ss.get(c, 0) - rs.get(c, 0)

    if missing_sim:
        print(f"  [warn] {missing_sim}/{n1*n2} sim results missing", file=sys.stderr)
    if missing_rtl:
        print(f"  [warn] {missing_rtl}/{n1*n2} RTL results missing", file=sys.stderr)

    return dict(ipc_err=ipc_err, rtl_ipc=rtl_ipc, sim_ipc=sim_ipc,
                stall_err=stall_err,
                labels1=labels1, labels2=labels2,
                name1=axis1["name"], name2=axis2["name"])


# ── Subplot drawing ────────────────────────────────────────────────────────────

def _annotate(ax, data, fmt, vmin=None, vmax_=None):
    """Write cell text values."""
    if vmin is None:
        vmin  = np.nanmin(data)
        vmax_ = np.nanmax(data)
    for yi in range(data.shape[0]):
        for xi in range(data.shape[1]):
            v = data[yi, xi]
            if not np.isnan(v):
                brightness = (v - vmin) / max(vmax_ - vmin, 1e-9)
                color = 'white' if brightness < 0.5 else 'black'
                ax.text(xi, yi, format(v, fmt),
                        ha='center', va='center', fontsize=8,
                        fontweight='bold', color=color)


def _axis_labels(ax, labels1, labels2, name1, name2):
    ax.set_xticks(range(len(labels2)))
    ax.set_xticklabels(labels2, fontsize=8)
    ax.set_yticks(range(len(labels1)))
    ax.set_yticklabels(labels1, fontsize=8)
    ax.set_xlabel(name2, fontsize=9)
    ax.set_ylabel(name1, fontsize=9)


def draw_error_ax(ax, data, labels1, labels2, name1, name2, title):
    if np.all(np.isnan(data)):
        ax.text(0.5, 0.5, "No RTL data", ha='center', va='center',
                transform=ax.transAxes, color='gray', fontsize=11)
        ax.set_title(title, fontsize=10)
        return
    vmax = max(abs(np.nanmin(data)), abs(np.nanmax(data)), 1.0)
    norm = TwoSlopeNorm(vmin=-vmax, vcenter=0, vmax=vmax)
    im = ax.imshow(data, cmap='RdYlGn_r', norm=norm, aspect='auto')
    _axis_labels(ax, labels1, labels2, name1, name2)
    ax.set_title(title, fontsize=10)
    plt.colorbar(im, ax=ax, shrink=0.85, label="IPC Error (%)")
    for yi in range(data.shape[0]):
        for xi in range(data.shape[1]):
            v = data[yi, xi]
            if not np.isnan(v):
                color = 'white' if abs(v) > vmax * 0.6 else 'black'
                ax.text(xi, yi, f"{v:+.1f}%",
                        ha='center', va='center', fontsize=8,
                        fontweight='bold', color=color)


def draw_ipc_ax(ax, data, labels1, labels2, name1, name2, title, label="IPC"):
    if np.all(np.isnan(data)):
        ax.text(0.5, 0.5, "No data", ha='center', va='center',
                transform=ax.transAxes, color='gray', fontsize=11)
        ax.set_title(title, fontsize=10)
        return
    im = ax.imshow(data, cmap='viridis', aspect='auto')
    _axis_labels(ax, labels1, labels2, name1, name2)
    ax.set_title(title, fontsize=10)
    plt.colorbar(im, ax=ax, shrink=0.85, label=label)
    vmin, vmax_ = np.nanmin(data), np.nanmax(data)
    _annotate(ax, data, ".4f", vmin, vmax_)


# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(
        description="npSim vs RTL IPC error heatmap",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    p.add_argument("--sim-dir",  required=True,
                   help="npSim simout sweep dir (simout/<name>)")
    p.add_argument("--rtl-dir",  required=True,
                   help="RTL sweep dir ($NPC_HOME/ccout/sweep-...)")
    p.add_argument("--prefix",   required=True,
                   help="Subdir prefix used during npSim sweep (e.g. 'coremark')")
    p.add_argument("--npc-mode", action="store_true",
                   help="Annotate title: NPC mode (no functional difference)")
    p.add_argument("--outfile",  default=None,
                   help="Output PNG (default: visual/plots/<sim-dir>/error_heatmap.png)")
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
    rtl_dir = Path(args.rtl_dir)
    if not rtl_dir.is_absolute():
        npc_home = Path(os.environ.get("NPC_HOME",
                                       str(NPSIM_HOME.parent / "npc")))
        rtl_dir = npc_home / args.rtl_dir

    if args.outfile:
        outfile = (NPSIM_HOME / args.outfile
                   if not Path(args.outfile).is_absolute()
                   else Path(args.outfile))
    else:
        tag = Path(args.sim_dir).name
        outfile = NPSIM_HOME / "visual" / "plots" / tag / "error_heatmap.png"
    outfile.parent.mkdir(parents=True, exist_ok=True)

    n_panels = len(enabled)
    mode_tag = " [NPC mode]" if args.npc_mode else " [SoC mode]"
    title_base = (args.title
                  or f"npSim vs RTL: {args.prefix} / {sim_dir.name}{mode_tag}")

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
                               sim_dir, rtl_dir, args.prefix)
        if result is None:
            print(f"  [warn] no {comp_name} axis found in {conf_path}",
                  file=sys.stderr)
            continue

        l1, l2, n1, n2 = (result["labels1"], result["labels2"],
                          result["name1"],   result["name2"])

        draw_error_ax(axes[0, col], result["ipc_err"],
                      l1, l2, n1, n2,
                      f"{comp_name.upper()} IPC Error (%)")

    plt.tight_layout(rect=[0, 0, 1, 0.94])
    plt.savefig(outfile, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved {outfile}")


if __name__ == "__main__":
    main()
