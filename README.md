# npSim

`npsim` is a trace-driven RV32IM timing simulator for the ysyxSoC/NPC flow.
It replays a sanitized NEMU trace through a configurable in-order pipeline,
L1 caches, branch predictor, and SoC memory system. The intended workflow is:

1. sweep RTL once in `npc/` to get ground truth
2. calibrate `npsim/` against those results
3. use `npsim` for fast design-space exploration and plotting

For the `23-Mar-2026` SoC calibration matrix on `ARCH=riscv32im-ysyxsoc`:

- `CoreMark (cm2)`: `52/52` cases are within `5%` IPC error
- `Dhrystone (dry2500)`: `48/52` cases are within `5%`, worst case `9.57%`

The current model is intentionally SoC-oriented. It follows the address routing
in `$SOC_HOME/ysyxSoC/perip`, models SDRAM in controller-device cycles, and
converts between `us`, `MHz`, and CPU cycles explicitly.

## Highlights

- Fast trace replay: much faster than repeated RTL sweeps once RTL ground truth exists
- Calibrated SoC model: pipeline, PLRU L1s, branch predictor, SDRAM/SRAM/MMIO routing
- ROI-aware: honors `SysResetStats`, `SysFenceI`, and `SysDumpStats` markers from the trace
- Sweep-friendly: shared config naming with RTL, parallel npSim-only sweep script
- Plotting included: cache error heatmaps, BPU miss-rate heatmaps, cycle-breakdown charts

## Build

```bash
cd $NPSIM_HOME
make clean && make -j8 all
```

`npsim` does not track header dependencies in the Makefile. After editing a
header, rebuild with `make clean && make -j8 all`.

## Quick Start

Single-run example:

```bash
cd $NPSIM_HOME
./build/npsim.elf tests/cm2-im-optklib2.nptr.zst \
  --freq-mhz 1000 \
  --l1i-size 2048 --l1i-blksize 16 --l1i-assoc 4 \
  --l1d-size 2048 --l1d-blksize 16 --l1d-assoc 4 \
  --bpu-type bimodal --bpu-size 256 --btb-size 128 --ras-size 8 \
  --l1i-pref none --l1d-pref none \
  --sdram-lat-us 0.051 --sdram-burst-us 0.024 \
  --sram-lat 1 --mmio-lat 3 --axi-ovhd-cyc 4 \
  --outdir quick-cm2
```

Outputs:

- `simout/<outdir>/conf.json`
- `simout/<outdir>/stats.json`

Use `--dry-run` to dump configuration without simulating.

## Trace-Driven Replay

`npsim` does not execute instructions functionally. It consumes a NEMU trace and
reconstructs timing from the recorded instruction stream:

- `TraceReader` reads compressed `.nptr.zst`
- `TraceSanitizer` checks basic trace consistency and records ROI summary
- `Pipeline` replays the 5-stage in-order core
- `PipeCache` and `BranchUnit` rebuild front-end / memory timing
- `RAMArbiter` routes each request to SDRAM, SRAM, CLINT, or other MMIO

The runtime obeys AM markers embedded in the trace:

- `SysResetStats`: reset simulator counters at ROI start
- `SysFenceI`: flush L1 state when the workload executes `fence.i`
- `SysDumpStats`: dump ROI stats and stop feeding new instructions

This matches the intended “measure only reset-stats to dump-stats” window.

## Architecture

The core model is a single-issue 5-stage pipeline:

```text
IFU -> IDU -> EXU -> LSU -> WBU
```

Main pieces:

- `src/pipeSim/Pipeline.{hh,cc}`: fetch/decode/execute/memory/writeback timing
- `src/cacheSim/CacheBase.{hh,cc}`: iCache, dCache, StoreBuffer, NoCache
- `src/branchSim/*`: Bimodal, GShare, Tournament, TAGE, BTB, RAS
- `src/cacheSim/RamConn.hh`: SoC address routing and RAM device interface
- `src/cacheSim/RamModel.hh`: SDRAM timing model
- `src/trace.{hh,cc}`: trace decode, sanitizer, SysOp markers

The SoC memory path is:

```text
CPU host -> RAMArbiter -> { SDRAM | SRAM | CLINT | OTHER }
```

This is deliberately closer to SoC mode than to a flat “single RAM latency”
model.

## Memory Model And Units

There are two layers of time in the SoC model:

- user-visible CLI knobs: `--sdram-lat-us`, `--sdram-burst-us`, `--freq-mhz`
- internal SDRAM controller model: fixed `100 MHz` device-domain timing

Conversion at the CLI boundary:

```text
cpu_cycles = ceil(lat_us * freq_mhz)
```

Example:

- `0.051 us` at `1000 MHz` becomes `51` CPU cycles
- `0.024 us` at `500 MHz` becomes `12` CPU cycles

Inside `SdramModel`, row-hit / row-miss / row-conflict timing is modeled in
device cycles, then scaled back to CPU cycles. This keeps the SoC latency model
grounded in controller timing instead of tuning arbitrary per-benchmark cycle
constants.

## Key CLI Knobs

Commonly used options:

| Option | Meaning |
|---|---|
| `--l1i-size --l1i-blksize --l1i-assoc` | iCache geometry |
| `--l1d-size --l1d-blksize --l1d-assoc` | dCache geometry |
| `--bpu-type --bpu-size --btb-size --ras-size` | branch predictor geometry |
| `--freq-mhz` | CPU frequency for `us -> cycles` conversion |
| `--sdram-lat-us --sdram-burst-us` | SDRAM latency knobs in microseconds |
| `--sram-lat` | on-chip SRAM latency in CPU cycles |
| `--mmio-lat` | fallback MMIO / OTHER device latency |
| `--axi-ovhd-cyc` | fixed CPU-side AXI overhead |
| `--l1i-pref --l1d-pref` | prefetcher type (`none`, `nextline`, `stride`, `tagged`) |
| `--l1i-repl --l1d-repl` | replacement policy (`plru`, `lru`, `srrip`, `rr`) |
| `--l1i-cwf` | critical-word-first iCache response |
| `--outdir` | output directory under `simout/` |
| `--print-none` | suppress terminal stat dump |
| `--dry-run` | config only, no simulation |

Notes:

- IFU fetch queue size is fixed in code to match RTL: `8`
- There is no branch-penalty tuning knob in the current model
- For the `23-Mar-2026` calibration sweep, both L1 prefetchers are disabled

## SoC Calibration Flow

The `23-Mar-2026` sweep uses a shared config source:

- shared matrix: `misc/soc_cal_23mar2026_common.py`
- RTL results: `npc/ccout/23-Mar-2026-Cal`
- npSim sweep: `npsim/scripts/run_soc_cal.py`

### 1. Run npSim Sweep

```bash
cd $NPSIM_HOME
python3 scripts/run_soc_cal.py \
  --out-root 24-Mar-2026-Cal-r5-full \
  --jobs 8 \
  --benches cm2,dry2500 \
  --freqs 500,1000
```

This script is npSim-only. It never triggers RTL compile or RTL run.

Outputs go to:

```text
npsim/simout/24-Mar-2026-Cal-r5-full/
  cm2-500MHz/<canonical-suffix>/
  cm2-1000MHz/<canonical-suffix>/
  dry2500-500MHz/<canonical-suffix>/
  dry2500-1000MHz/<canonical-suffix>/
```

### 2. Summarize Error

```bash
cd $NPSIM_HOME
python3 scripts/report_soc_cal.py \
  --sim-root $NPSIM_HOME/simout/24-Mar-2026-Cal-r5-full \
  --rtl-root $NPC_HOME/ccout/23-Mar-2026-Cal
```

Current summary:

- `cm2-500MHz`: max `4.76%`
- `cm2-1000MHz`: max `4.08%`
- `dry2500-500MHz`: max `5.39%`
- `dry2500-1000MHz`: max `9.57%`

### 3. Draw Figures

```bash
cd $NPSIM_HOME
python3 visual/plot_soc_cal_23mar.py \
  --sim-root $NPSIM_HOME/simout/24-Mar-2026-Cal-r5-full \
  --rtl-root $NPC_HOME/ccout/23-Mar-2026-Cal \
  --outdir $NPSIM_HOME/visual/plots/23-Mar-2026-Cal
```

Generated figures include:

- cache breakdown error heatmaps for group `00`, `01`, `03`
- BPU miss-rate absolute-error heatmaps for group `04`
- cycle breakdown stacked bars for each benchmark/frequency pair

Generated today at:

- `visual/plots/23-Mar-2026-Cal/`

## Plotting Output

The current plotting script writes:

```text
visual/plots/23-Mar-2026-Cal/
  cm2-500MHz-group00-cache-breakdown.png
  cm2-500MHz-group01-cache-breakdown.png
  cm2-500MHz-group03-cache-breakdown.png
  cm2-500MHz-group04-bpu-missrate-abs.png
  cm2-500MHz-cycle-breakdown.png
  ...
```

Interpretation:

- cache heatmap cells: `abs((sim - rtl) / rtl) * 100`
- BPU heatmap cells: `abs(sim_miss_rate - rtl_miss_rate)` in percentage points
- cycle charts: stacked share of `NoStall`, `NoInst`, `LsuStall`, `BranchMispred`, `RAW`

## Source Tree

```text
src/
  main.cc                     CLI, wiring, simulation loop
  trace.{hh,cc}               trace reader + sanitizer + SysOp handling
  pipeSim/Pipeline.{hh,cc}    5-stage in-order timing core
  cacheSim/CacheBase.{hh,cc}  L1 caches, NoCache, StoreBuffer
  cacheSim/RamConn.hh         RAMArbiter + SoC address map
  cacheSim/RamModel.hh        SDRAM timing model
  branchSim/*                 branch predictor and BTB implementations

scripts/
  run_soc_cal.py              shared SoC calibration sweep
  report_soc_cal.py           compare npSim against RTL

visual/
  plot_perf.py                generic performance heatmap
  plot_error_heatmap.py       generic RTL-vs-sim heatmap
  plot_soc_cal_23mar.py       figures for the current SoC sweep
```

## Current Cleanup Notes

Recent cleanup for this calibration pass:

- fixed dCache victim/writeback mismatch so eviction stats match RTL
- switched L1 replacement default to PLRU to match RTL
- routed RAM traffic through `RAMArbiter -> {SDRAM, SRAM, CLINT, OTHER}`
- removed the dead `stq_size` plumbing from the pipeline constructor
- kept the model aligned with real RTL knobs instead of adding ghost timing parameters
