# npSim — Trace-Driven RISC-V Microarchitecture Simulator

Replays NEMU instruction traces through a configurable pipeline,
cache, and memory model. Calibrated against NPC RTL (≤3.4% IPC,
≤2% area error across 128B–1kB iCache sweep).

## Quick Reference

```bash
make all -j4 DEBUG_MODE=0 NPSIM_ACTIVE=1       # Release build
./build/npsim.elf <trace.nptr.zst> [options]    # Run simulation
python3 scripts/sweep_2d.py --conf <config.py> --outdir <dir> --jobs 4
python3 area/area_est.py --conf-json simout/<dir>/conf.json --outdir simout/<dir>
```

### NPC mode (standalone PMemBox)

```bash
./build/npsim.elf trace.nptr.zst --npc-mode \
  --l1i-size 512B --l1i-blksize 16 --sdram-lat 43 --sdram-burst-lat 16 \
  --ifq-size 4 --stbuf-entries 0 --br-pen 1 -O my-run
```

### SoC mode (default, ysyxSoC XBar + SDRAM)

```bash
./build/npsim.elf trace.nptr.zst \
  --l1i-size 512B --l1i-blksize 16 --sdram-lat 51 --sdram-burst-lat 24 \
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
| `--sdram-lat` | `43` | SDRAM first-beat latency |
| `--sdram-burst-lat` | `16` | SDRAM per-beat burst latency |
| `--sram-lat` | `1` | SoC on-chip SRAM latency |
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
| `sdram_lat` | 43 | 51 |
| `sdram_burst_lat` | 16 | 24 |
| `sram_lat` | — | 1 |
| `ifq_size` | 4 | 3 |
| `br_pen` | 1 | 1 |

### IPC results (128B–1kB × 16B/32B, CoreMark)

**NPC mode** (sdram=43/16, ifq=4): max ±3.4%.
**SoC mode** (sdram=51/24, ifq=3): max ±3.3%.

### Area results (DFF and SRAM modes)

**DFF** (`--sram-dff`): max ±2.0%.
**SRAM** (`--sram-lib`): max ±2.0%.

Area model: Core=15630µm², iCache=575+264×line_words µm² (control),
DFF=5.226µm²/bit, SRAM=0.346/0.55 µm²/bit, comb=15% (logic only).

### Reproduce

```bash
# NPC IPC sweep
python3 scripts/sweep_2d.py --conf scripts/sweep_configs/npc_cal_feb27.py \
  --outdir 27Feb-npc --jobs 4 --no-area

# SoC IPC sweep
python3 scripts/sweep_2d.py --conf scripts/sweep_configs/soc_cal_feb27.py \
  --outdir 27Feb-soc --jobs 4 --no-area
```

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
├── sweep_2d.py              2D parameter sweep
├── compare_rtl.py           RTL comparison heatmap
└── sweep_configs/           Sweep config files
area/
└── area_est.py              Offline area estimation from conf.json
```
