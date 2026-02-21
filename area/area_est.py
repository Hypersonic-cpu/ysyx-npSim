#!/usr/bin/env python3
"""Parse npSim config JSON, estimate area, output area composition.

Uses analytical SRAM area model since CACTI is unavailable on aarch64.
Area estimates at NanGate 45nm technology node.
"""

import argparse
import json
import sys
from pathlib import Path

TECH_NM = 45

# 6T SRAM cell area at 45nm: ~0.346 um² (NanGate45 lib)
SRAM_CELL_UM2 = 0.346

# Typical SRAM array efficiency: 50-70% (the rest is decoders, sense amps, etc)
SRAM_EFFICIENCY = 0.55

# DFF area per bit at 45nm: ~5 um²
DFF_PER_BIT = 5.0


def sram_area(bits):
    """Estimate SRAM array area including peripheral logic."""
    cell_area = bits * SRAM_CELL_UM2
    return cell_area / SRAM_EFFICIENCY


def cache_area(size_bytes, block_bytes, assoc):
    """Estimate cache area: data array + tag array."""
    data_bits = size_bytes * 8

    num_sets = size_bytes // (block_bytes * assoc)
    # Tag bits per line: 32 - log2(sets) - log2(block_size) + valid + dirty
    import math
    idx_bits = int(math.log2(num_sets)) if num_sets > 1 else 0
    off_bits = int(math.log2(block_bytes)) if block_bytes > 1 else 0
    tag_bits_per_line = (32 - idx_bits - off_bits) + 2  # valid + dirty
    num_lines = size_bytes // block_bytes
    tag_bits = num_lines * tag_bits_per_line

    data_area = sram_area(data_bits)
    tag_area = sram_area(tag_bits)
    return data_area, tag_area


def estimate_component(name, conf, outdir):
    """Estimate area for one component from its config JSON."""
    area_conf = conf.get("area")
    if area_conf is None:
        return {"name": name, "total_um2": 0.0}

    timing_area = area_conf.get("timing_area", 0.0)
    comb_pct = area_conf.get("comb_percent", 0.0)
    cacti_objs = area_conf.get("cacti_objs", [])

    sram_total = 0.0
    sram_details = {}

    for obj in cacti_objs:
        label = obj.get("label", "unknown")
        t = obj["type"]
        size = obj["size"]

        if size < 1:
            continue

        if t == "cache":
            data_a, tag_a = cache_area(
                size, obj["block_size"], obj["assoc"]
            )
            area_um2 = data_a + tag_a
            sram_details[label] = {
                "data_um2": round(data_a, 1),
                "tag_um2": round(tag_a, 1),
                "total_um2": round(area_um2, 1),
            }
        else:
            # RAM: simple SRAM array
            area_um2 = sram_area(size * 8)
            sram_details[label] = round(area_um2, 1)

        sram_total += area_um2

    total = timing_area + sram_total
    if comb_pct > 0 and total > 0:
        total = total / (1.0 - comb_pct)

    return {
        "name": name,
        "total_um2": round(total, 1),
        "timing_area_um2": round(timing_area, 1),
        "sram_area_um2": round(sram_total, 1),
        "comb_area_um2": round(total - timing_area - sram_total, 1),
        "sram_details": sram_details,
    }


def process_nested_branch(name, conf, outdir):
    """Handle BranchUnit which nests bpu_config and btb_config."""
    results = []
    results.append(estimate_component(name, conf, outdir))

    bpu_conf = conf.get("bpu_config", {})
    if bpu_conf:
        bpu_name = conf.get("bpu", "BPU")
        results.append(estimate_component(bpu_name, bpu_conf, outdir))

    btb_conf = conf.get("btb_config", {})
    if btb_conf:
        btb_name = conf.get("btb", "BTB")
        results.append(estimate_component(btb_name, btb_conf, outdir))

    return results


def main():
    parser = argparse.ArgumentParser(
        description="Estimate chip area from npSim config JSON"
    )
    parser.add_argument("--conf-json", required=True,
                        help="Path to npSim output JSON file")
    parser.add_argument("--out-dir", required=True,
                        help="Output directory for results")
    args = parser.parse_args()

    with open(args.conf_json) as f:
        data = json.load(f)

    config = data.get("config", {})
    outdir = Path(args.out_dir)
    outdir.mkdir(parents=True, exist_ok=True)

    all_results = []
    grand_total = 0.0

    for comp_name, comp_conf in config.items():
        if not isinstance(comp_conf, dict):
            continue

        if "bpu_config" in comp_conf:
            results = process_nested_branch(comp_name, comp_conf, outdir)
            for r in results:
                all_results.append(r)
                grand_total += r["total_um2"]
        else:
            r = estimate_component(comp_name, comp_conf, outdir)
            all_results.append(r)
            grand_total += r["total_um2"]

    output = {
        "source": args.conf_json,
        "technology_nm": TECH_NM,
        "model": "analytical (6T SRAM cell 0.346um², efficiency 55%)",
        "total_area_um2": round(grand_total, 1),
        "total_area_mm2": round(grand_total / 1e6, 6),
        "components": all_results,
    }

    out_json = outdir / "area_comp.json"
    with open(out_json, "w") as f:
        json.dump(output, f, indent=2)

    print(f"Total area: {grand_total:.0f} um² ({grand_total/1e6:.6f} mm²)")
    for r in all_results:
        pct = r["total_um2"] / grand_total * 100 if grand_total > 0 else 0
        print(f"  {r['name']:20s}: {r['total_um2']:8.0f} um²  ({pct:5.1f}%)")
    print(f"Written to {out_json}")


if __name__ == "__main__":
    main()
