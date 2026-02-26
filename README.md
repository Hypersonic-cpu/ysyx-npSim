# npSim — Trace-Driven RISC-V Microarchitecture Simulator

npSim replays NEMU instruction traces through a configurable pipeline,
cache hierarchy, branch predictor, and memory model. It produces
cycle-approximate performance metrics (IPC, miss rates, stall breakdowns)
calibrated against the NPC RTL within a few percent.

TODOs:
- [x] Calibrate SoC mode with RTL on Microbench and CoreMark.
- [ ] Manual stats check (cache access, mispred etc) with RTL.
- [ ] Remove ACTIVE_MODE compile option. Isolate from npc/.
- [ ] Fix false `NoCache` logic.

## Building

Requires **clang++-22** with **libc++** and C++23 support.
Environment variable `NPSIM_HOME` must point to this directory.
Libraries (`nlohmann/json`, `stats_template`) are bundled in `libs/`.

```bash
make all -j4 DEBUG_MODE=0 NPSIM_ACTIVE=1   # Release build → build/npsim.elf
make all -j4 DEBUG_MODE=1 NPSIM_ACTIVE=1   # Debug build (enables DPRINTF)
```

Two modes exist:
- **Active mode** (`NPSIM_ACTIVE=1`): standalone executable
- **Passive mode**: compiled as a library linked into `npc/` RTL simulation

## Running

```bash
./build/npsim.elf <trace_file> [options]
```

### CLI Options

| Flag | Default | Description |
|------|---------|-------------|
| **iCache** | | |
| `--l1i-size` | `1kB` | iCache total size (supports `256B`, `1kB`, `4kB`) |
| `--l1i-blksize` | `16` | iCache line (block) size in bytes |
| `--l1i-assoc` | `1` | iCache associativity |
| **dCache** | | |
| `--l1d-size` | `0` | dCache size (0 = no dCache) |
| `--stbuf-entries` | `2` | StoreBuffer entries (used when no dCache) |
| **BPU** | | |
| `--bpu-type` | `none` | Predictor: `bimodal`, `gshare`, `tournament`, `alwaystaken`, `btfnt`, `none` |
| `--bpu-size` | `16` | BPU table entries |
| `--btb-size` | `16` | BTB entries |
| **Memory** | | |
| `--sdram-lat` | `45` | SDRAM first-beat latency (cycles) |
| `--sdram-burst-lat` | `10` | SDRAM per-beat burst latency |
| `--npc-mode` | off | Use NPC mode (disables default SoC address routing) |
| `--sram-lat` | `1` | SoC: on-chip SRAM latency |
| **Area model** | | |
| `--sram-dff` | on | Area model uses DFF-per-bit estimate |
| `--sram-lib` | off | Area model uses OpenRAM SRAM macro estimate |
| **Pipeline** | | |
| `--ifq-size` | `3` | Instruction fetch queue depth |
| `--br-pen` | `1` | Branch misprediction penalty (cycles) |
| **Output** | | |
| `--outdir` | — | Write stats to `simout/<outdir>/stats.json` |
| `--max-ticks` | ∞ | Stop after N ticks |
| `--debug-flags` | — | Comma-separated debug flags |

### SoC Mode (default)

SoC mode is enabled by default. It routes memory requests through
address-based latency classification (SRAM vs SDRAM), matching the
ysyxSoC XBar + SDRAM controller path.

```bash
./build/npsim.elf tests/coremark-soc-cal.nptr.zst \
  --l1i-size 1kB --l1i-blksize 16 --l1i-assoc 1 \
  --l1d-size 0 --bpu-type none --stbuf-entries 0 --br-pen 1 \
  --sdram-lat 55 --sdram-burst-lat 23 --sram-lat 1 \
  --ifq-size 3 --outdir my-soc-run
```

### NPC Mode

Pass `--npc-mode` to disable SoC address routing and use a flat SDRAM
latency model, matching the NPC Verilator setup (PMemBox backend).

```bash
./build/npsim.elf tests/coremark-npc-cal2.nptr.zst \
  --l1i-size 1kB --l1i-blksize 16 --l1i-assoc 1 \
  --l1d-size 0 --bpu-type none --stbuf-entries 0 --br-pen 1 \
  --sdram-lat 45 --sdram-burst-lat 10 --ifq-size 3 \
  --npc-mode --outdir my-run
```

## Generating Traces

Traces are 16-byte packed `TraceInst` records (`.nptr.zst` = zstd-compressed)
containing PC, memory address, register indices, branch outcome, and
system-ops (reset/dump stats).

```bash
cd $AM_BENCH/coremark
# Use the same binary as RTL.
make ARCH=riscv32e-ysyxsoc mainargs="" insert-arg
# Emulate with proper device config. (-b must go first)
$NEMU_HOME/build/riscv32-nemu-interpreter -b IMAGE_FILE_PATH --nptr OUTPUT_FILE.nptr.zst
```

For MicroBench, set `mainargs="train"` (or `"test"`). For NPC mode, use `ARCH=riscv32e-npc`.

## Module Layout

```
src/
├── main.cc                  CLI, component wiring, simulation loop
├── trace.{cc,hh}            TraceInst, TraceReader (zstd/xz), TraceSanitizer
├── pipeSim/
│   ├── Pipeline.hh          5-stage pipeline: types, stats, config
│   └── Pipeline.cc          Fetch/Decode/Execute/Memory/WriteBack logic
├── cacheSim/
│   ├── CacheBase.{hh,cc}    PipeCache (pipelined), NoCache, StoreBuffer
│   ├── CacheLine.{hh,cc}    Tag/valid/dirty cache line
│   ├── Prefetcher.hh        NextLine, Stride, Tagged prefetchers
│   └── RamConn.{hh,cc}      RAMArbiter — memory port arbiter
├── branchSim/
│   ├── BranchPred.hh        BranchUnit + predictor variants
│   └── BranchPred.cc        Bimodal, GShare, Tournament, NoBPU, …
├── areaSim/AreaEst.hh       area_json() / sram_cache() / sram_ram() helpers
└── defines/
    ├── mode_ctrl.{cc,hh}    g_soc_mode global (default: true = SoC)
    ├── base.hh              SimObject / ClockedObject base classes
    ├── types.hh             addr_t, word_t, tick_t, tint_t
    ├── interface.hh         CpuTrans, AckTrans, MemTrans
    └── debug.{hh,cc}        Debug flag infrastructure
scripts/
├── sweep_2d.py              2D parameter sweep driver
├── compare_rtl.py           RTL vs Sim IPC comparison + heatmap
└── sweep_configs/            Sweep config files (Python modules)
```

## Simulation Architecture

### Event-Driven Tick Loop

Each `ClockedObject` reports its next-active tick via `next_update()`.
The main loop advances a global tick counter; `do_update()` calls
`update_impl()` only when the tick matches. Bottom-up order:
**SDRAM → dCache → iCache → Pipeline**.

### Pipeline Model (`pipeSim/Pipeline`)

5-stage in-order: **Fetch → Decode → Execute → Memory → WriteBack**.

```
  IFU ──► IDU ──► EXU ──► LSU ──► WBU
   │                       │
  iCache              NoCache/StoreBuffer
   │                       │
   └────── RAMArbiter ─────┘
```

**Instruction Fetch Queue (IFQ):** The IFU issues iCache read requests
and pushes entries into a FIFO (depth = `ifq_size`, default 3). When the
iCache responds, the entry's `wait_mem` flag clears and it can enter the
Fetch pipeline stage. This models the RTL FetchStage's pipelined buffer.

**RAW Hazard Tracking:** A per-register `reg_ready_[32]` array records
when each register will be available. Decode stalls if any source
register isn't ready. Matches RTL forwarding policy:
- EXU: no forwarding (gprFw = false)
- LSU: forwards ALU results only (wbSel == fromAlu)
- WBU: always forwards

**Stall Attribution:** Every cycle is attributed to exactly one cause:
`NoStall` (committed instruction), `NoInst` (iCache miss / fetch stall),
`LsuStall` (load/store in flight), `BrMispred` (branch recovery),
`RAW` (data hazard).

### Branch Misprediction Model

At IF stage, the simulator already knows the branch outcome from the
trace. If the prediction is wrong:

1. **T+0:** Branch enters IF. Set `in_wrong_path_` = true.
   The IFU starts fetching from wrong-path PCs (pc+4 or predicted
   target), polluting the iCache — matching RTL behavior.

2. **T+1 to T+N:** Wrong-path fetches fill the IFQ. Behavior depends
   on mode:
   - **NPC mode:** Wrong-path entries stay in IFQ until EX flushes.
     This limits wrong-path iCache pollution to IFQ_SIZE requests.
   - **SoC mode:** IDU drains completed wrong-path entries at 1/cycle,
     freeing IFQ slots for additional wrong-path fetches. This matches
     SoC RTL behavior where high SDRAM latency naturally rate-limits
     the effective wrong-path pollution rate.

3. **T+2:** Branch reaches EX. `do_execute()` triggers flush:
   - Flush all IFQ entries, track orphan iCache responses
   - Set `fetch_resume_tick_` = T+2 + `BranchMissPenalty`

4. **T+2+penalty:** First correct-path fetch issues.

### Cache Model (`cacheSim/PipeCache`)

Pipelined set-associative cache with configurable pipeline depth
(default 3 stages for iCache, 2 for dCache). Each cycle, entries
shift through the pipe:

- **Hit:** Response returned 1 cycle after entry reaches pipe back
  (models RTL word-select register stage).
- **Miss:** Two-cycle deferred fill request:
  - Cycle 1: Set `pending_fill_req_`
  - Cycle 2: Fire AXI AR to memory arbiter, set `is_replay_` = true
  - When memory responds: fill line, block for +2 cycles, replay access
- **Speculative access:** Wrong-path fetches call `read_req_speculative()`.
  These probe the cache but do not allocate on miss — the miss responds
  immediately without memory fill or cache stall. This avoids polluting
  the cache with wrong-path data while allowing speculative hits.

`NoCache` is a simple pass-through (used for dCache when `l1d-size=0`).
`StoreBuffer` is a FIFO write buffer for stores when no dCache exists.

### Memory Arbiter (`memSim::RAMArbiter`)

Models the AXI arbiter with separate read and write channels.
Higher-indexed hosts have higher priority (dCache > iCache).

**Latency model:**
```
  Total = sdram_lat + (burst_len - 1) × sdram_burst_lat
```
For a single-beat access (e.g., LSU word load), burst_len = 1, so
total = `sdram_lat`.

In **SoC mode** (default), addresses are classified:
- `isSRAM(addr)`: 0x0f000000–0x0fffffff → `sram_lat` (1 cycle)
- `isCLINT(addr)`: 0x02000000–0x0200ffff → `sram_lat` (1 cycle)
- Everything else (SDRAM): standard burst formula

**NPC mode** (`--npc-mode`) disables address routing; all accesses use
the SDRAM burst formula regardless of address.

### Memory Port Contention

When both iCache and dCache issue requests to the same channel (R or W),
the arbiter services the higher-priority host first. The lower-priority
host must wait until the channel is free. This creates structural hazards
that naturally increase `NoInst` stall cycles when the dCache or LSU
is actively using the memory port.

## Stats Output

JSON structure: `{ "config": {...}, "stats0": {...}, "stats1": {...} }`.
`SysResetStats` / `SysDumpStats` trace ops create stats windows.
Key metric: `stats0.Core.ipc`. `TraceSanitizer` counts instruction
types (loads, stores, branches, ALU ops) for trace validation.

## Parameter Sweeps and RTL Calibration

### Running Sweeps

```bash
python3 scripts/sweep_2d.py \
  --conf scripts/sweep_configs/npc_cal.py \
  --outdir npc-cal --jobs 3 --no-area
```

Config files in `scripts/sweep_configs/` define `trace`, `axis1`, `axis2`,
and `default_conf`. Results go to `simout/<outdir>/`.

### RTL Comparison

```bash
python3 scripts/compare_rtl.py \
  --rtl-dir $NPC_HOME/ccout/sweep-cache-coremark \
  --sim-dir simout/npc-cal \
  --outfile visual/plots/npc-cal/rtl_cmp.png
```

Generates IPC error heatmap and cache hit/miss comparison table.

### Calibration Process

The calibration aligns npSim IPC with RTL Verilator results across an
iCache sweep (size ∈ {128B, 256B, 512B, 1kB} × blksize ∈ {16, 32}).

**Step 1: Area calibration.** Compare yosys-sta area reports against
npSim area estimates for both DFF and SRAM macro modes. Tuning
parameter: `DFF_PER_BIT` in `area/area_est.py` (calibrated to 5.226).
DFF mode matches within ±4%, SRAM mode within ±9%.

**Step 2: Timing calibration.** Run RTL Verilator CoreMark for each
cache config (`make compile DIFFENA=0 RTL_SCALA_ARG="--l1i-size X
--l1i-blksize Y"`) and compare against npSim with matching parameters
(`--stbuf-entries 0 --sdram-lat 40 --sdram-burst-lat 8 --br-pen 1
--npc-mode`). Key fixes applied:
- Speculative (non-allocating) wrong-path cache access
- Branch misprediction stall attribution (2 cycles per misprediction)
- Correct orphan tracking for in-flight cache requests at EX flush

### Calibrated Parameters

| Parameter | NPC Mode | SoC Mode | Rationale |
|-----------|----------|----------|-----------|
| `sdram_lat` | 40 | 55 | SDRAM first-beat (NPC PMemBox FSM ≈40 cycles) |
| `sdram_burst_lat` | 8 | 23 | Per subsequent beat |
| `sram_lat` | — | 1 | On-chip SRAM (1 cycle) |
| `stbuf_entries` | 0 | 0 | RTL StoreBuffer currently disabled |
| `br_pen` | 1 | 1 | 1-cycle fetch resume delay after EX flush |

NPC mode models a simple PMemBox (DPI-C latency 40/8). SoC mode models
the full ysyxSoC XBar + SDRAM controller path.

### Current Calibration Results

**NPC CoreMark** (3-cycle iCache pipeline, stbuf=0, no BPU):

IPC error = (npSim − RTL) / RTL.

| iCache | Line | RTL IPC | npSim IPC | Error |
|--------|------|---------|-----------|-------|
| 128B | 16B | 0.0645 | 0.0842 | +30.7% |
| 128B | 32B | 0.0584 | 0.0839 | +43.7% |
| 256B | 16B | 0.0698 | 0.0907 | +29.9% |
| 256B | 32B | 0.0625 | 0.0903 | +44.5% |
| 512B | 16B | 0.0992 | 0.1141 | +15.0% |
| 512B | 32B | 0.0944 | 0.1133 | +20.1% |
| 1024B | 16B | 0.1194 | 0.1248 | +4.5% |
| 1024B | 32B | 0.1181 | 0.1243 | +5.2% |

Accuracy improves with larger caches: ≤5% for 1kB, ≤20% for 512B.
Small caches (128–256B) show +30–45% error.

#### Error Analysis

The dominant error source is **wrong-path cache pollution**. In RTL,
mispredicted fetches use BTB-predicted addresses (real code locations)
that allocate cache lines on miss, evicting useful data from the
working set. In npSim, wrong-path fetches use synthetic sequential PCs
(pc+4) with non-allocating speculative access, preserving the cache.

This causes npSim to underestimate cache misses, especially for small
caches where the working set barely fits. The effect is most pronounced
at 128B (8–4 sets) and diminishes as cache size grows past the
instruction working set.

Per-category stall breakdown (1024B/32B, best case):

| Category | RTL | npSim | Error |
|----------|-----|-------|-------|
| NoStall | 11.81% | 12.43% | +5.2% |
| NoInst | 13.89% | 14.43% | +3.9% |
| LsuStall | 67.23% | 66.42% | −1.2% |
| BrMispred | 4.58% | 5.01% | +9.4% |
| RAW | 2.49% | 1.72% | −30.9% |

RAW is consistently underestimated (~30%) due to differences in
register forwarding timing granularity.

**SoC CoreMark** (12/12 configs, ≤4.9% error):

| iCache Size | 8B line | 16B line | 32B line |
|-------------|---------|----------|----------|
| 256B | −1.19% | −2.44% | −4.92% |
| 512B | +0.21% | +0.11% | −1.39% |
| 1kB | −0.68% | −0.05% | −0.85% |
| 4kB | +0.77% | +0.49% | −0.02% |
