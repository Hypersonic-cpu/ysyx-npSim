#!/usr/bin/env python3
"""Generate BPU RTL performance and calibration plots."""

import json
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

NPSIM_HOME = Path(__file__).resolve().parent.parent.parent
NPC_HOME = NPSIM_HOME.parent / "npc"

RTL_RESULTS = NPC_HOME / "simout" / "bpu_sweep" / "results.json"
CALI_RESULTS = NPC_HOME / "simout" / "bpu_cali" / "cali_results.json"
AREA_DIR = NPSIM_HOME / "areaout" / "bpu_cali"

PERF_DIR = Path(__file__).parent / "bpu_rtl_perf"
CALI_DIR = Path(__file__).parent / "bpu_cali"

BTB_SIZES = [32, 64, 128, 256]
RAS_SIZES = [0, 4]
BP_TYPES = ["bimodal", "btfnt"]
COLORS = {"bimodal": "#2196F3", "btfnt": "#FF9800"}
RAS_HATCHES = {0: "", 4: "//"}

RTL_AREA = {
    ("bimodal", 32, 0): 93101, ("bimodal", 32, 4): 94230,
    ("bimodal", 64, 0): 104903, ("bimodal", 64, 4): 106244,
    ("bimodal", 128, 0): 128465, ("bimodal", 128, 4): 129660,
    ("bimodal", 256, 0): 172180, ("bimodal", 256, 4): 175056,
    ("btfnt", 32, 0): 92633, ("btfnt", 32, 4): 93700,
    ("btfnt", 64, 0): 103691, ("btfnt", 64, 4): 104795,
    ("btfnt", 128, 0): 125734, ("btfnt", 128, 4): 127101,
    ("btfnt", 256, 0): 168313, ("btfnt", 256, 4): 170057,
}

RTL_AREA_SRAM = {
    ("bimodal", 32, 0): 30432, ("bimodal", 32, 4): 31761,
    ("bimodal", 64, 0): 32593, ("bimodal", 64, 4): 34284,
    ("bimodal", 128, 0): 36742, ("bimodal", 128, 4): 39366,
    ("bimodal", 256, 0): 45194, ("bimodal", 256, 4): 49116,
    ("btfnt", 32, 0): 29963, ("btfnt", 32, 4): 31347,
    ("btfnt", 64, 0): 31424, ("btfnt", 64, 4): 33040,
    ("btfnt", 128, 0): 34289, ("btfnt", 128, 4): 36694,
    ("btfnt", 256, 0): 40072, ("btfnt", 256, 4): 43939,
}

NPSIM_AREA_SRAM = {
    ("bimodal", 32, 0): 31409, ("bimodal", 32, 4): 32656,
    ("bimodal", 64, 0): 33406, ("bimodal", 64, 4): 34892,
    ("bimodal", 128, 0): 37360, ("bimodal", 128, 4): 39323,
    ("bimodal", 256, 0): 45186, ("bimodal", 256, 4): 48105,
    ("btfnt", 32, 0): 30932, ("btfnt", 32, 4): 32178,
    ("btfnt", 64, 0): 32450, ("btfnt", 64, 4): 33936,
    ("btfnt", 128, 0): 35448, ("btfnt", 128, 4): 37412,
    ("btfnt", 256, 0): 41364, ("btfnt", 256, 4): 44283,
}


def load_rtl():
    with open(RTL_RESULTS) as f:
        return json.load(f)


def load_cali():
    with open(CALI_RESULTS) as f:
        return json.load(f)


def load_npsim_area():
    areas = {}
    for bp in BP_TYPES:
        for btb in BTB_SIZES:
            for ras in RAS_SIZES:
                tag = f"{bp}_btb{btb}_ras{ras}"
                p = AREA_DIR / tag / "area_comp.json"
                if p.exists():
                    with open(p) as f:
                        d = json.load(f)
                    areas[(bp, btb, ras)] = d["total_area_um2"]
    return areas


def plot_rtl_ipc(rtl_data):
    fig, ax = plt.subplots(figsize=(8, 4.5))
    x = np.arange(len(BTB_SIZES))
    w = 0.18
    offsets = {
        ("bimodal", 0): -1.5, ("bimodal", 4): -0.5,
        ("btfnt", 0): 0.5, ("btfnt", 4): 1.5,
    }
    lookup = {d["tag"]: d for d in rtl_data}
    for bp in BP_TYPES:
        for ras in RAS_SIZES:
            vals = []
            for btb in BTB_SIZES:
                tag = f"{bp}_btb{btb}_ras{ras}"
                vals.append(lookup[tag]["ipc"])
            label = f"{bp} RAS={ras}"
            off = offsets[(bp, ras)]
            bars = ax.bar(x + off * w, vals, w,
                          color=COLORS[bp], alpha=0.9 if ras == 4 else 0.5,
                          hatch=RAS_HATCHES[ras], edgecolor="black",
                          linewidth=0.5, label=label)
    ax.set_xticks(x)
    ax.set_xticklabels([str(s) for s in BTB_SIZES])
    ax.set_xlabel("BTB Entries")
    ax.set_ylabel("IPC")
    ax.set_title("RTL IPC vs BPU Configuration (CoreMark SoC)")
    ax.legend(fontsize=8, ncol=2)
    ax.set_ylim(0.155, 0.170)
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(PERF_DIR / "rtl_ipc.png", dpi=150)
    plt.close(fig)


def plot_rtl_bp_accuracy(rtl_data):
    fig, ax = plt.subplots(figsize=(8, 4.5))
    x = np.arange(len(BTB_SIZES))
    w = 0.18
    offsets = {
        ("bimodal", 0): -1.5, ("bimodal", 4): -0.5,
        ("btfnt", 0): 0.5, ("btfnt", 4): 1.5,
    }
    lookup = {d["tag"]: d for d in rtl_data}
    for bp in BP_TYPES:
        for ras in RAS_SIZES:
            vals = []
            for btb in BTB_SIZES:
                tag = f"{bp}_btb{btb}_ras{ras}"
                vals.append(lookup[tag]["bp_accuracy"] * 100)
            label = f"{bp} RAS={ras}"
            off = offsets[(bp, ras)]
            ax.bar(x + off * w, vals, w,
                   color=COLORS[bp], alpha=0.9 if ras == 4 else 0.5,
                   hatch=RAS_HATCHES[ras], edgecolor="black",
                   linewidth=0.5, label=label)
    ax.set_xticks(x)
    ax.set_xticklabels([str(s) for s in BTB_SIZES])
    ax.set_xlabel("BTB Entries")
    ax.set_ylabel("BP Accuracy (%)")
    ax.set_title("Branch Prediction Accuracy (CoreMark SoC)")
    ax.legend(fontsize=8, ncol=2)
    ax.set_ylim(65, 85)
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(PERF_DIR / "bp_accuracy.png", dpi=150)
    plt.close(fig)


def plot_rtl_mispred_breakdown(rtl_data):
    fig, axes = plt.subplots(1, 2, figsize=(10, 4.5))
    lookup = {d["tag"]: d for d in rtl_data}
    for ax_i, bp in enumerate(BP_TYPES):
        ax = axes[ax_i]
        x = np.arange(len(BTB_SIZES))
        w = 0.35
        for ri, ras in enumerate(RAS_SIZES):
            btb_miss = [lookup[f"{bp}_btb{b}_ras{ras}"]["btb_miss"]
                        for b in BTB_SIZES]
            wrong_dir = [lookup[f"{bp}_btb{b}_ras{ras}"]["wrong_dir"]
                         for b in BTB_SIZES]
            wrong_tgt = [lookup[f"{bp}_btb{b}_ras{ras}"]["wrong_tgt"]
                         for b in BTB_SIZES]
            off = (ri - 0.5) * w
            ax.bar(x + off, btb_miss, w, label="BTB Miss" if ri == 0 else "",
                   color="#E57373", hatch=RAS_HATCHES[ras], edgecolor="black",
                   linewidth=0.5)
            ax.bar(x + off, wrong_dir, w, bottom=btb_miss,
                   label="Wrong Dir" if ri == 0 else "",
                   color="#FFB74D", hatch=RAS_HATCHES[ras], edgecolor="black",
                   linewidth=0.5)
            bot2 = [a + b for a, b in zip(btb_miss, wrong_dir)]
            ax.bar(x + off, wrong_tgt, w, bottom=bot2,
                   label="Wrong Tgt" if ri == 0 else "",
                   color="#64B5F6", hatch=RAS_HATCHES[ras], edgecolor="black",
                   linewidth=0.5)
        ax.set_xticks(x)
        ax.set_xticklabels([str(s) for s in BTB_SIZES])
        ax.set_xlabel("BTB Entries")
        ax.set_ylabel("Mispredictions")
        ax.set_title(f"{bp.upper()}")
        if ax_i == 0:
            ax.legend(fontsize=7)
    fig.suptitle("Misprediction Breakdown (hatched = RAS4)", fontsize=11)
    fig.tight_layout()
    fig.savefig(PERF_DIR / "mispred_breakdown.png", dpi=150)
    plt.close(fig)


def plot_cali_ipc(cali_data, rtl_data):
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(10, 4.5))
    lookup_rtl = {d["tag"]: d for d in rtl_data}
    lookup_cali = {d["tag"]: d for d in cali_data}

    rtl_ipcs, sim_ipcs, labels = [], [], []
    for d in cali_data:
        rtl_ipcs.append(d["rtl_ipc"])
        sim_ipcs.append(d["sim_ipc"])
        labels.append(d["tag"])

    ax1.scatter(rtl_ipcs, sim_ipcs, c=["#2196F3" if "bimodal" in l else "#FF9800"
                                        for l in labels], s=40, zorder=5)
    mn, mx = min(rtl_ipcs + sim_ipcs) * 0.99, max(rtl_ipcs + sim_ipcs) * 1.01
    ax1.plot([mn, mx], [mn, mx], "k--", alpha=0.5, linewidth=0.8)
    ax1.set_xlabel("RTL IPC")
    ax1.set_ylabel("npSim IPC")
    ax1.set_title("IPC: RTL vs npSim")
    ax1.set_xlim(mn, mx)
    ax1.set_ylim(mn, mx)
    ax1.set_aspect("equal")
    ax1.grid(alpha=0.3)

    errs = [d["ipc_err_pct"] for d in cali_data]
    x = np.arange(len(errs))
    colors = ["#2196F3" if "bimodal" in d["tag"] else "#FF9800" for d in cali_data]
    ax2.bar(x, errs, color=colors, edgecolor="black", linewidth=0.5)
    ax2.axhline(y=5, color="red", linestyle="--", alpha=0.7, label="5% target")
    ax2.set_xticks(x)
    ax2.set_xticklabels([d["tag"].replace("_", "\n") for d in cali_data],
                         fontsize=5, rotation=45, ha="right")
    ax2.set_ylabel("IPC Error (%)")
    ax2.set_title("IPC Calibration Error")
    ax2.legend(fontsize=8)
    ax2.set_ylim(0, 6)
    ax2.grid(axis="y", alpha=0.3)

    fig.tight_layout()
    fig.savefig(CALI_DIR / "ipc_calibration.png", dpi=150)
    plt.close(fig)


def plot_cali_area(npsim_areas):
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(10, 4.5))

    rtl_vals, sim_vals, labels, colors = [], [], [], []
    errs = []
    for bp in BP_TYPES:
        for btb in BTB_SIZES:
            for ras in RAS_SIZES:
                key = (bp, btb, ras)
                if key in RTL_AREA and key in npsim_areas:
                    rv = RTL_AREA[key]
                    sv = npsim_areas[key]
                    rtl_vals.append(rv / 1000)
                    sim_vals.append(sv / 1000)
                    err = abs(sv - rv) / rv * 100
                    errs.append(err)
                    labels.append(f"{bp}_btb{btb}_ras{ras}")
                    colors.append(COLORS[bp])

    ax1.scatter(rtl_vals, sim_vals, c=colors, s=40, zorder=5)
    mn = min(rtl_vals + sim_vals) * 0.95
    mx = max(rtl_vals + sim_vals) * 1.05
    ax1.plot([mn, mx], [mn, mx], "k--", alpha=0.5, linewidth=0.8)
    ax1.set_xlabel("RTL Area (×1000 µm²)")
    ax1.set_ylabel("npSim Area (×1000 µm²)")
    ax1.set_title("DFF Area: RTL vs npSim")
    ax1.set_xlim(mn, mx)
    ax1.set_ylim(mn, mx)
    ax1.set_aspect("equal")
    ax1.grid(alpha=0.3)

    x = np.arange(len(errs))
    ax2.bar(x, errs, color=colors, edgecolor="black", linewidth=0.5)
    ax2.axhline(y=5, color="red", linestyle="--", alpha=0.7, label="5% target")
    ax2.set_xticks(x)
    ax2.set_xticklabels([l.replace("_", "\n") for l in labels],
                         fontsize=5, rotation=45, ha="right")
    ax2.set_ylabel("Area Error (%)")
    ax2.set_title("DFF Area Calibration Error")
    ax2.legend(fontsize=8)
    ax2.set_ylim(0, 6)
    ax2.grid(axis="y", alpha=0.3)

    fig.tight_layout()
    fig.savefig(CALI_DIR / "area_calibration.png", dpi=150)
    plt.close(fig)


def plot_cali_area_sram():
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(10, 4.5))

    rtl_vals, sim_vals, labels, colors = [], [], [], []
    errs = []
    for bp in BP_TYPES:
        for btb in BTB_SIZES:
            for ras in RAS_SIZES:
                key = (bp, btb, ras)
                if key in RTL_AREA_SRAM and key in NPSIM_AREA_SRAM:
                    rv = RTL_AREA_SRAM[key]
                    sv = NPSIM_AREA_SRAM[key]
                    rtl_vals.append(rv / 1000)
                    sim_vals.append(sv / 1000)
                    err = abs(sv - rv) / rv * 100
                    errs.append(err)
                    labels.append(f"{bp}_btb{btb}_ras{ras}")
                    colors.append(COLORS[bp])

    ax1.scatter(rtl_vals, sim_vals, c=colors, s=40, zorder=5)
    mn = min(rtl_vals + sim_vals) * 0.95
    mx = max(rtl_vals + sim_vals) * 1.05
    ax1.plot([mn, mx], [mn, mx], "k--", alpha=0.5, linewidth=0.8)
    ax1.set_xlabel("RTL Area (×1000 µm²)")
    ax1.set_ylabel("npSim Area (×1000 µm²)")
    ax1.set_title("SRAM Area: RTL vs npSim")
    ax1.set_xlim(mn, mx)
    ax1.set_ylim(mn, mx)
    ax1.set_aspect("equal")
    ax1.grid(alpha=0.3)

    x = np.arange(len(errs))
    ax2.bar(x, errs, color=colors, edgecolor="black", linewidth=0.5)
    ax2.axhline(y=5, color="red", linestyle="--", alpha=0.7, label="5% target")
    ax2.set_xticks(x)
    ax2.set_xticklabels([l.replace("_", "\n") for l in labels],
                         fontsize=5, rotation=45, ha="right")
    ax2.set_ylabel("Area Error (%)")
    ax2.set_title("SRAM Area Calibration Error")
    ax2.legend(fontsize=8)
    ax2.set_ylim(0, 6)
    ax2.grid(axis="y", alpha=0.3)

    fig.tight_layout()
    fig.savefig(CALI_DIR / "area_calibration_sram.png", dpi=150)
    plt.close(fig)


def plot_dff_vs_sram_area():
    fig, ax = plt.subplots(figsize=(8, 5))
    x = np.arange(len(BTB_SIZES))
    w = 0.1
    offsets = {
        ("bimodal", 0, "dff"): -3.5, ("bimodal", 0, "sram"): -2.5,
        ("bimodal", 4, "dff"): -1.5, ("bimodal", 4, "sram"): -0.5,
        ("btfnt", 0, "dff"): 0.5, ("btfnt", 0, "sram"): 1.5,
        ("btfnt", 4, "dff"): 2.5, ("btfnt", 4, "sram"): 3.5,
    }
    for bp in BP_TYPES:
        for ras in RAS_SIZES:
            dff_vals = [RTL_AREA[(bp, btb, ras)] / 1000
                        for btb in BTB_SIZES]
            sram_vals = [RTL_AREA_SRAM[(bp, btb, ras)] / 1000
                         for btb in BTB_SIZES]
            off_d = offsets[(bp, ras, "dff")]
            off_s = offsets[(bp, ras, "sram")]
            ax.bar(x + off_d * w, dff_vals, w,
                   color=COLORS[bp], alpha=0.4,
                   edgecolor="black", linewidth=0.5,
                   label=f"{bp} R{ras} DFF" if ras == 0 else "")
            ax.bar(x + off_s * w, sram_vals, w,
                   color=COLORS[bp], alpha=0.9,
                   hatch="//", edgecolor="black", linewidth=0.5,
                   label=f"{bp} R{ras} SRAM" if ras == 0 else "")
    ax.set_xticks(x)
    ax.set_xticklabels([str(s) for s in BTB_SIZES])
    ax.set_xlabel("BTB Entries")
    ax.set_ylabel("Area (×1000 µm²)")
    ax.set_title("DFF vs SRAM Area (RTL, 1kB iCache)")
    ax.legend(fontsize=7, ncol=2)
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(PERF_DIR / "dff_vs_sram_area.png", dpi=150)
    plt.close(fig)


def plot_rtl_area_vs_ipc(rtl_data):
    fig, ax = plt.subplots(figsize=(7, 5))
    lookup = {d["tag"]: d for d in rtl_data}
    for bp in BP_TYPES:
        for ras in RAS_SIZES:
            areas, ipcs, btbs = [], [], []
            for btb in BTB_SIZES:
                tag = f"{bp}_btb{btb}_ras{ras}"
                key = (bp, btb, ras)
                if tag in lookup and key in RTL_AREA:
                    areas.append(RTL_AREA[key] / 1000)
                    ipcs.append(lookup[tag]["ipc"])
                    btbs.append(btb)
            marker = "o" if ras == 0 else "s"
            label = f"{bp} RAS={ras}"
            ax.plot(areas, ipcs, marker=marker, color=COLORS[bp],
                    label=label, linewidth=1.5, markersize=6)
            for a, ipc, b in zip(areas, ipcs, btbs):
                ax.annotate(str(b), (a, ipc), fontsize=6,
                            textcoords="offset points", xytext=(4, 4))
    ax.set_xlabel("Area (×1000 µm²)")
    ax.set_ylabel("IPC")
    ax.set_title("RTL Area vs IPC Trade-off (CoreMark SoC)")
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(PERF_DIR / "area_vs_ipc.png", dpi=150)
    plt.close(fig)


def main():
    PERF_DIR.mkdir(parents=True, exist_ok=True)
    CALI_DIR.mkdir(parents=True, exist_ok=True)

    rtl_data = load_rtl()
    cali_data = load_cali()
    npsim_areas = load_npsim_area()

    plot_rtl_ipc(rtl_data)
    plot_rtl_bp_accuracy(rtl_data)
    plot_rtl_mispred_breakdown(rtl_data)
    plot_rtl_area_vs_ipc(rtl_data)
    plot_dff_vs_sram_area()
    plot_cali_ipc(cali_data, rtl_data)
    plot_cali_area(npsim_areas)
    plot_cali_area_sram()

    print("Generated plots:")
    for d in [PERF_DIR, CALI_DIR]:
        for f in sorted(d.glob("*.png")):
            print(f"  {f.relative_to(NPSIM_HOME)}")


if __name__ == "__main__":
    main()
