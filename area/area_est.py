#!/usr/bin/env python3
"""Parse npSim config JSON, estimate area, output area composition.

SRAM area is estimated via CACTI (NanGate 45nm) or analytical 6T cell model.
DFF (timing_bits) and STA (known_area) use the NanGate 45nm DFF_X1 constant.
"""

import argparse
import json
import math
import re
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
CACTI_DIR = SCRIPT_DIR.parent / "libs" / "cacti"
CACTI_BIN = CACTI_DIR / "cacti"
TECH_UM = 0.045
TECH_NM = 45

# NanGate 45nm DFF_X1: ~5 um² per bit
DFF_PER_BIT = 5.0

# 6T SRAM cell model at 45nm (analytical fallback when CACTI is unavailable)
SRAM_CELL_UM2 = 0.346   # 6T cell area
SRAM_EFFICIENCY = 0.55  # cell area / total array area (includes peripheral)

# Minimum SRAM size (bytes) for CACTI
CACTI_MIN_BYTES = 128


def _analytical_sram_area(size_bytes):
    """Estimate SRAM area using 6T cell model (data bits only; no tags)."""
    bits = size_bytes * 8
    return round(bits * SRAM_CELL_UM2 / SRAM_EFFICIENCY, 1)


def _tag_area_analytical(size, block, assoc):
    """Estimate tag array area using 6T SRAM cell model (45nm).
    Returns (area_um2, tag_bits)."""
    num_sets = size // (block * assoc)
    num_lines = size // block
    off_bits = int(math.log2(block)) if block > 1 else 0
    idx_bits = int(math.log2(num_sets)) if num_sets > 1 else 0
    tag_bits_per_line = max(32 - off_bits - idx_bits, 1) + 2  # +valid+dirty
    total_tag_bits = num_lines * tag_bits_per_line
    area = total_tag_bits * SRAM_CELL_UM2 / SRAM_EFFICIENCY
    return round(area, 1), total_tag_bits


def gen_cacti_cfg(outpath, size, block, assoc, is_cache=True):
    """Generate a CACTI .cfg file. Returns path, or None if too small."""
    if size < CACTI_MIN_BYTES:
        return None

    # For caches, the access port is one CPU word (32 bits for RV32),
    # not the full block. For RAM tables, each access reads one entry.
    bus_width = 32 if is_cache else block * 8
    cache_type = '"cache"' if is_cache else '"ram"'

    lines = [
        f"-size (bytes) {size}",
        f"-block size (bytes) {block}",
        f"-associativity {assoc}",
        "-read-write port 1",
        "-exclusive read port 0",
        "-exclusive write port 0",
        "-single ended read ports 0",
        "-UCA bank count 1",
        f"-technology (u) {TECH_UM}",
        "-page size (bits) 8192",
        "-burst length 8",
        "-internal prefetch width 8",
        '-Data array cell type - "itrs-hp"',
        '-Data array peripheral type - "itrs-hp"',
        '-Tag array cell type - "itrs-hp"',
        '-Tag array peripheral type - "itrs-hp"',
        f"-output/input bus width {bus_width}",
        "-operating temperature (K) 350",
        f"-cache type {cache_type}",
        '-tag size (b) "default"',
        '-access mode (normal, sequential, fast) - "normal"',
        "-design objective (weight delay, dynamic power, leakage power, "
        "cycle time, area) 0:0:0:100:0",
        "-deviate (delay, dynamic power, leakage power, cycle time, area) "
        "20:100000:100000:100000:100000",
        "-NUCAdesign objective (weight delay, dynamic power, leakage power, "
        "cycle time, area) 100:100:0:0:100",
        "-NUCAdeviate (delay, dynamic power, leakage power, cycle time, "
        "area) 10:10000:10000:10000:10000",
        '-Optimize ED or ED^2 (ED, ED^2, NONE): "NONE"',
        '-Cache model (NUCA, UCA)  - "UCA"',
        "-NUCA bank count 0",
        '-Wire signaling (fullswing, lowswing, default) - "Global_30"',
        '-Wire inside mat - "semi-global"',
        '-Wire outside mat - "semi-global"',
        '-Interconnect projection - "conservative"',
        "-Core count 1",
        '-Cache level (L2/L3) - "L2"',
        '-Add ECC - "true"',
        '-Print level (DETAILED, CONCISE) - "DETAILED"',
        '-Print input parameters - "true"',
        '-Force cache config - "false"',
        "-Ndwl 1", "-Ndbl 1", "-Nspd 0",
        "-Ndcm 1", "-Ndsam1 0", "-Ndsam2 0",
    ]
    with open(outpath, "w") as f:
        f.write("\n".join(lines) + "\n")
    return outpath


def run_cacti(cfg_path):
    """Run CACTI and parse area output (mm² → um²).
    CACTI uses relative paths for tech_params/, so we must run from its dir.
    """
    cfg_abs = Path(cfg_path).resolve()
    try:
        result = subprocess.run(
            [str(CACTI_BIN), "-infile", str(cfg_abs)],
            capture_output=True, text=True, timeout=30,
            cwd=str(CACTI_DIR)
        )
    except (subprocess.TimeoutExpired, FileNotFoundError) as e:
        print(f"  CACTI error: {e}", file=sys.stderr)
        return None

    data_area = 0.0
    tag_area = 0.0
    for line in result.stdout.splitlines():
        m = re.match(r"\s+Data array: Area \(mm2\):\s+([\d.e+-]+)", line)
        if m:
            v = float(m.group(1))
            if not math.isnan(v):
                data_area = v
        m = re.match(r"\s+Tag array: Area \(mm2\):\s+([\d.e+-]+)", line)
        if m:
            v = float(m.group(1))
            if not math.isnan(v):
                tag_area = v

    if data_area == 0.0 and tag_area == 0.0:
        return None  # CACTI failed to produce results
    return {
        "data_um2": round(data_area * 1e6, 1),
        "tag_um2": round(tag_area * 1e6, 1),
        "total_um2": round((data_area + tag_area) * 1e6, 1),
    }


def estimate_sram(name, label, obj, outdir):
    """Estimate SRAM area for one cacti_obj descriptor.

    Supports two type values (and legacy names for backward compatibility):
      "sram" / "cache" / "ram"  →  SRAM macro (SyncReadMem in RTL)
        - has block_size + assoc  → CACTI cache mode
        - has word_size only      → CACTI RAM mode
        Falls back to analytical 6T SRAM cell model if CACTI unavailable.
        Never falls back to DFF (DFF is only for explicitly declared timing_bits).

    The CACTI minimum is CACTI_MIN_BYTES; below that, CACTI is skipped and
    the analytical model is used directly.
    """
    t = obj.get("type", "sram")
    size = obj["size"]
    if size < 1:
        return 0.0, {}

    has_cache_params = "block_size" in obj and "assoc" in obj

    if has_cache_params:
        # ── Cache mode (iCache / dCache) ─────────────────────────────────
        block = obj["block_size"]
        assoc = obj["assoc"]
        cfg_path = outdir / f"cacti_{name}_{label}_cache.cfg"
        gen = gen_cacti_cfg(cfg_path, size, block, assoc, is_cache=True)
        if gen is not None:
            result = run_cacti(cfg_path)
            if result is not None:
                result["obj_type"] = "sram_cache"
                result["mode"] = "cacti_cache"
                return result["total_um2"], result

        # Analytical fallback: data array + tag array
        data_area = _analytical_sram_area(size)
        tag_area, tag_bits = _tag_area_analytical(size, block, assoc)
        total = data_area + tag_area
        return total, {
            "obj_type": "sram_cache",
            "mode": "sram_analytical",
            "data_um2": data_area,
            "tag_um2": tag_area,
            "tag_bits": tag_bits,
            "total_um2": round(total, 1),
        }

    else:
        # ── RAM mode (BTB, BPU tables) ────────────────────────────────────
        block = max(obj.get("word_size", 1), 1)
        for ram_block in sorted(set([block, block // 2, block // 4, 8, 4]),
                                reverse=True):
            if ram_block < 1:
                continue
            if size // ram_block < 16:
                continue
            cfg_path = outdir / f"cacti_{name}_{label}_ram{ram_block}.cfg"
            gen = gen_cacti_cfg(cfg_path, size, ram_block, 1, is_cache=False)
            if gen is not None:
                result = run_cacti(cfg_path)
                if result is not None:
                    result["obj_type"] = "sram_ram"
                    result["mode"] = "ram_ok"
                    return result["total_um2"], result

        # Analytical fallback: data bits only (no tags for RAM mode)
        area_um2 = _analytical_sram_area(size)
        return area_um2, {
            "obj_type": "sram_ram",
            "mode": "sram_analytical",
            "sram_analytical_um2": area_um2,
            "bits": size * 8,
        }


def estimate_component(name, conf, outdir):
    """Estimate area for one component from its config JSON."""
    area_conf = conf.get("area")
    if area_conf is None:
        return {"name": name, "total_um2": 0.0}

    known_area = area_conf.get("known_area", 0.0)
    # timing_bits: DFF bit count emitted by C++ (process-agnostic).
    # Backward compat: old JSONs have DFF area baked into known_area already,
    # so timing_bits defaults to 0 and has no effect on old-format files.
    timing_bits = area_conf.get("timing_bits", 0)
    dff_area = timing_bits * DFF_PER_BIT
    comb_pct = area_conf.get("comb_percent", 0.0)
    cacti_objs = area_conf.get("cacti_objs", [])

    sram_total = 0.0
    sram_details = {}

    for obj in cacti_objs:
        label = obj.get("label", "unknown")
        area_um2, detail = estimate_sram(name, label, obj, outdir)
        sram_details[label] = detail
        sram_total += area_um2

    seq_total = known_area + dff_area  # all sequential/logic area
    total = seq_total + sram_total
    if comb_pct > 0 and total > 0:
        total = total / (1.0 - comb_pct)

    return {
        "name": name,
        "total_um2": round(total, 1),
        "known_area_um2": round(known_area, 1),
        "dff_area_um2": round(dff_area, 1),
        "sram_area_um2": round(sram_total, 1),
        "comb_area_um2": round(total - seq_total - sram_total, 1),
        "sram_details": sram_details,
    }


def process_nested_branch(name, conf, outdir):
    """Handle BranchUnit which nests bpu_config and btb_config."""
    results = [estimate_component(name, conf, outdir)]

    bpu_conf = conf.get("bpu_config", {})
    if bpu_conf:
        results.append(estimate_component(
            conf.get("bpu", "BPU"), bpu_conf, outdir))

    btb_conf = conf.get("btb_config", {})
    if btb_conf:
        results.append(estimate_component(
            conf.get("btb", "BTB"), btb_conf, outdir))

    return results


def main():
    parser = argparse.ArgumentParser(
        description="Estimate chip area from npSim config JSON"
    )
    parser.add_argument("--conf-json", required=True,
                        help="Path to npSim output JSON file")
    parser.add_argument("--outdir", required=True,
                        help="Output directory for results")
    args = parser.parse_args()

    with open(args.conf_json) as f:
        data = json.load(f)

    config = data.get("config", {})
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    all_results = []
    grand_total = 0.0

    for comp_name, comp_conf in config.items():
        if not isinstance(comp_conf, dict):
            continue
        if "bpu_config" in comp_conf:
            for r in process_nested_branch(comp_name, comp_conf, outdir):
                all_results.append(r)
                grand_total += r["total_um2"]
        else:
            r = estimate_component(comp_name, comp_conf, outdir)
            all_results.append(r)
            grand_total += r["total_um2"]

    output = {
        "source": args.conf_json,
        "technology_nm": TECH_NM,
        "model": "CACTI 7.0 (45nm); analytical 6T SRAM fallback; DFF via timing_bits",
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
