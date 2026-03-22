# npSim — Trace-Driven RISC-V Microarchitecture Simulator

Replays NEMU instruction traces through a configurable pipeline,
cache, and memory model. Calibrated against NPC RTL (≤3.4% IPC,
≤0.65% area error across 128B–1kB iCache sweep).

## Quick Reference

```bash
make all -j4 DEBUG_MODE=0       # Release build
./build/npsim.elf <trace.nptr.zst> [options]    # Run simulation
python3 scripts/sweep_2d.py --conf <config.py> --outdir <dir> --jobs 4
python3 area/area_est.py --conf-json simout/<dir>/conf.json --outdir simout/<dir>
```

### NPC mode (standalone PMemBox)

```bash
./build/npsim.elf trace.nptr.zst --npc-mode \
  --l1i-size 512B --l1i-blksize 16 --sdram-lat-us 0.043 --sdram-burst-us 0.016 \
  --ifq-size 4 --stbuf-entries 0 --br-pen 1 -O my-run
```

### SoC mode (default, ysyxSoC XBar + SDRAM)

```bash
./build/npsim.elf trace.nptr.zst \
  --l1i-size 512B --l1i-blksize 16 --sdram-lat-us 0.051 --sdram-burst-us 0.024 \
  --sram-lat 1 --ifq-size 3 --stbuf-entries 0 --br-pen 1 -O my-run
```

### Area estimation

```bash
./build/npsim.elf trace.nptr.zst --dry-run --sram-dff -O area-dff  # DFF mode
./build/npsim.elf trace.nptr.zst --dry-run --sram-lib -O area-sram # SRAM mode
python3 area/area_est.py --conf-json simout/area-dff/conf.json --outdir simout/area-dff
```

### Generating traces

```bash
cd $AM_BENCH/coremark
make ARCH=riscv32e-ysyxsoc mainargs="" insert-arg
$NEMU_HOME/build/riscv32-nemu-interpreter -b IMAGE --nptr OUTPUT.nptr.zst
```

## CLI Options

| Flag | Default | Description |
|------|---------|-------------|
| `--l1i-size` | `1kB` | iCache size (`128B`, `256B`, `1kB`, `4kB`) |
| `--l1i-blksize` | `16` | iCache line size in bytes |
| `--l1i-assoc` | `1` | iCache associativity |
| `--l1d-size` | `0` | dCache size (0 = disabled) |
| `--stbuf-entries` | `0` | StoreBuffer entries |
| `--bpu-type` | `none` | `bimodal`/`gshare`/`tournament`/`none` |
| `--sdram-lat-us` | `0.043` | SDRAM first-beat latency (µs; ×1000 = cycles @ 1 GHz) |
| `--sdram-burst-us` | `0.016` | SDRAM per-beat burst latency (µs) |
| `--sram-lat` | `1` | SoC on-chip SRAM latency (cycles) |
| `--freq-mhz` | `1000` | CPU frequency for µs→cycle conversion |
| `--npc-mode` | off | Flat SDRAM model (no address routing) |
| `--ifq-size` | `4` | Fetch queue depth (RTL PipeDepth+1) |
| `--br-pen` | `1` | Branch misprediction penalty |
| `--sram-dff` | on | Area: DFF-per-bit model |
| `--sram-lib` | off | Area: OpenRAM SRAM macro model |
| `--outdir` | — | Output to `simout/<dir>/` |
| `--dry-run` | off | Config only (no simulation) |

## Calibration

### Parameters

| Parameter | NPC Mode | SoC Mode |
|-----------|----------|----------|
| `sdram-lat-us` | 0.043 | 0.051 |
| `sdram-burst-us` | 0.016 | 0.024 |
| `sram-lat` | — | 1 |
| `ifq_size` | 4 | 3 |
| `br_pen` | 1 | 1 |

### IPC results (128B–1kB × 16B/32B, CoreMark)

**NPC mode** (sdram=43/16, ifq=4): max ±3.4%.
**SoC mode** (sdram=51/24, ifq=3): max ±3.3%.

### Area results (DFF and SRAM modes)

**DFF** (`--sram-dff`): max ±0.65%.
**SRAM** (`--sram-lib`): max ±0.65%.

Area model: Core=14730µm², iCache=575+264×line_words µm² (control),
DFF=5.226µm²/bit, comb=15%. SRAM=0.346/0.55 µm²/bit, comb=31%.

### Reproduce

```bash
# NPC IPC sweep
python3 scripts/sweep_2d.py --conf scripts/sweep_configs/npc_cal_feb27.py \
  --prefix coremark --outdir 27Feb-npc --jobs 4 --no-area

# SoC IPC sweep
python3 scripts/sweep_2d.py --conf scripts/sweep_configs/soc_cal_feb27.py \
  --prefix coremark --outdir 27Feb-soc --jobs 4 --no-area
```

## Sweep & Visualization

### Directory naming

Simulation output and RTL reference directories use a canonical format:

```
{prefix}_l1i-{sz}-b{blk}-a{assoc}[_l1d-{sz}-b{blk}-a{assoc}][_bpu-{type}-h{bht}-t{btb}]
```

Example: `coremark_l1i-1024-b16-a1_l1d-512-b16-a1_bpu-bimodal-h64-t64`

Sizes are always in bytes (integers). The `--prefix` argument to `sweep_2d.py`
enables canonical naming; omit for legacy `axis_name-val` format.

### Sweep config format

Sweep configs are Python modules in `scripts/sweep_configs/`. Two formats:

**Classic 2D** (`axis1`, `axis2`, `default_conf`): sweeps a single 2D grid.

**Multi-component** (`icache_axis`, `dcache_axis`, `bpu_axis`, `default_conf`):
generates the Cartesian product of all enabled components. Each component dict
has `axis1`, optional `axis2`, and `fixed` (applied to every run).

```python
# scripts/sweep_configs/example.py
trace = "tests/coremark-soc-ext.nptr.zst"

icache_axis = {
    "axis1": {"name": "l1i_size", "vals": [{"l1i-size": "512B"}, {"l1i-size": "1kB"}],
               "labels": ["512B", "1kB"]},
    "axis2": {"name": "l1i_blksize", "vals": [{"l1i-blksize": "16"}, {"l1i-blksize": "32"}],
               "labels": ["16B", "32B"]},
    "fixed": {"l1i-assoc": "1"},
}
dcache_axis = {
    "axis1": {"name": "l1d_size", "vals": [{"l1d-size": "512B"}, {"l1d-size": "1kB"}],
               "labels": ["512B", "1kB"]},
    "fixed": {"l1d-blksize": "16", "l1d-assoc": "1"},
}
bpu_axis = {
    "axis1": {"name": "btb_size",
               "vals": [{"bpu-type": "bimodal", "bpu-size": "64", "btb-size": "64"},
                        {"bpu-type": "bimodal", "bpu-size": "128", "btb-size": "128"}],
               "labels": ["btb64", "btb128"]},
    "fixed": {},
}
default_conf = {
    "stbuf-entries": "2", "br-pen": "1",
    "sdram-lat-us": "0.051", "sdram-burst-us": "0.024",
    "sram-lat": "1", "ifq-size": "3",
}
```

### Running a sweep

```bash
# Multi-component sweep (generates 4×2 × 2 × 3 = 48 dirs)
python3 scripts/sweep_2d.py \
    --conf   scripts/sweep_configs/cfg_ext_valid.py \
    --prefix cfg-ext-valid --outdir cfg-ext-valid \
    --no-area --jobs 8

# Classic 2D sweep with canonical naming
python3 scripts/sweep_2d.py \
    --conf scripts/sweep_configs/soc_cal.py \
    --prefix coremark --outdir soc-cal --jobs 4
```

### Plotting

```bash
# Performance heatmap (all 3 component panels)
python3 visual/plot_perf.py \
    --sim-dir simout/cfg-ext-valid --prefix cfg-ext-valid --metric ipc \
    --icache scripts/sweep_configs/cfg_ext_valid.py \
    --dcache scripts/sweep_configs/cfg_ext_valid.py \
    --bpu    scripts/sweep_configs/cfg_ext_valid.py \
    --outfile visual/plots/cfg-ext-valid/perf_ipc.png

# Error heatmap vs RTL (iCache panel only)
python3 visual/plot_error_heatmap.py \
    --sim-dir simout/soc-cal \
    --rtl-dir $NPC_HOME/ccout/sweep-cache-coremark-soc \
    --prefix  coremark \
    --icache  scripts/sweep_configs/soc_cal.py \
    --outfile visual/plots/soc-cal/error_heatmap.png
```

When plotting with a multi-component config, non-active components are fixed at
their **first** axis1 value (+ `fixed` params). Pass all relevant component
flags to ensure the canonical dir name matches what was simulated.

### Available configs

| Config | Format | Description |
|--------|--------|-------------|
| `npc_cal.py` | Classic 2D | NPC-mode iCache calibration (45/10 cycles) |
| `soc_cal.py` | Classic 2D | SoC-mode iCache calibration (55/23 cycles) |
| `npc_cal_feb27.py` | Classic 2D | NPC-mode calibration (43/16, ≤3.4% IPC error) |
| `soc_cal_feb27.py` | Classic 2D | SoC-mode calibration (51/24, ≤3.3% IPC error) |
| `cfg_ext_valid.py` | Multi-comp | Combined iCache×dCache×BPU SoC sweep |
| `bpu_vs_pf.py` | Classic 2D | BPU type vs prefetcher |
| `dcache_vs_pf.py` | Classic 2D | dCache size vs prefetcher |

## Architecture

5-stage in-order pipeline: Fetch → Decode → Execute → Memory → WriteBack.

```
IFU ──► IDU ──► EXU ──► LSU ──► WBU
 │                       │
iCache              StoreBuffer
 │                       │
 └────── RAMArbiter ─────┘
```

Wrong-path model: mispredicted fetches allocate iCache lines on miss
(matching RTL). IFQ limits pollution budget. No iCache pipe flush on
misprediction — fills complete.

Memory latency: `sdram_lat + (burst_len - 1) × sdram_burst_lat`.
SoC mode routes SRAM/CLINT addresses to `sram_lat`.

## Module Layout

```
src/
├── main.cc                  CLI, wiring, simulation loop
├── trace.{cc,hh}            TraceInst, TraceReader, TraceSanitizer
├── pipeSim/Pipeline.{hh,cc} 5-stage pipeline model
├── cacheSim/                PipeCache, NoCache, StoreBuffer, RAMArbiter
├── branchSim/BranchPred.*   Bimodal, GShare, Tournament, NoBPU
├── areaSim/AreaEst.hh       Area estimation helpers
└── defines/                 Base classes, types, debug flags
scripts/
├── sweep_2d.py              2D/multi-component parameter sweep
└── sweep_configs/           Sweep config files (Python modules)
visual/
├── plot_perf.py             Performance heatmap (IPC / miss rates)
├── plot_error_heatmap.py    IPC error heatmap vs RTL
└── plots/                   Generated figures
area/
└── area_est.py              Offline area estimation from conf.json
```
