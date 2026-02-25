# npSim — Trace-Driven RISC-V Microarchitecture Simulator

npSim replays NEMU instruction traces through a configurable pipeline,
cache hierarchy, branch predictor, and memory model. It produces
cycle-approximate performance metrics (IPC, miss rates, stall breakdowns)
calibrated against the NPC RTL within a few percent.

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
| `--socmode` | off | Enable SoC mode (address-based latency routing) |
| `--sram-lat` | `1` | SoC: on-chip SRAM latency |
| **Pipeline** | | |
| `--ifq-size` | `3` | Instruction fetch queue depth |
| `--br-pen` | `1` | Branch misprediction penalty (cycles) |
| **Output** | | |
| `--outdir` | — | Write stats to `simout/<outdir>/stats.json` |
| `--max-ticks` | ∞ | Stop after N ticks |
| `--debug-flags` | — | Comma-separated debug flags |

### NPC Mode (default)

```bash
./build/npsim.elf tests/coremark-npc-cal2.nptr.zst \
  --l1i-size 1kB --l1i-blksize 16 --l1i-assoc 1 \
  --l1d-size 0 --bpu-type none --stbuf-entries 0 --br-pen 1 \
  --sdram-lat 45 --sdram-burst-lat 10 --ifq-size 3 \
  --outdir my-run
```

### SoC Mode

```bash
./build/npsim.elf tests/coremark-soc-cal.nptr.zst \
  --l1i-size 1kB --l1i-blksize 16 --l1i-assoc 1 \
  --l1d-size 0 --bpu-type none --stbuf-entries 0 --br-pen 1 \
  --sdram-lat 55 --sdram-burst-lat 23 --sram-lat 1 \
  --socmode --ifq-size 3 --outdir my-soc-run
```

## Generating Traces

Traces are 16-byte packed `TraceInst` records (`.nptr.zst` = zstd-compressed)
containing PC, memory address, register indices, branch outcome, and
system-ops (reset/dump stats).

```bash
cd $AM_HOME/../benchmarks/coremark
make ARCH=riscv32e-nemu mainargs="" NEMUFLAGS="-b --nptr $(pwd)/build/coremark.nptr.zst" run
```

For MicroBench, set `mainargs="train"` (or `"test"`).

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
├── areaSim/AreaEst.hh       Chip area estimation helpers
└── defines/
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
(default 2 stages). Each cycle, entries shift through the pipe:

- **Hit:** Response returned when entry reaches pipe back. CPU blocked
  for 1 cycle (models RTL cache response latency).
- **Miss:** Two-cycle deferred fill request:
  - Cycle 1: Set `pending_fill_req_`
  - Cycle 2: Fire AXI AR to memory arbiter, set `is_replay_` = true
  - When memory responds: fill line, block for +2 cycles, replay access

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

In **SoC mode**, addresses are classified:
- `isSRAM(addr)`: 0x0f000000–0x0fffffff → `sram_lat` (1 cycle)
- `isCLINT(addr)`: 0x02000000–0x0200ffff → `sram_lat` (1 cycle)
- Everything else (SDRAM): standard burst formula

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
iCache sweep (size ∈ {256B, 512B, 1kB, 4kB} × blksize ∈ {8, 16, 32}).

**Step 1: Counter calibration.** Set very low memory latency so misses
are nearly free. Compare iCache hit/miss counts between npSim and RTL.
Ensure the wrong-path model generates similar amounts of iCache
pollution.

**Step 2: Memory latency calibration.** Tune `sdram_lat` and
`sdram_burst_lat` to match RTL's effective AMAT (average memory
access time). The RTL PMemBox FSM adds overhead per beat; the simulator
parameters absorb this.

**Step 3: IPC calibration.** Sweep all configs and compare IPC.
Targets: 256B ≤ 15%, 512B ≤ 10%, 1kB/4kB ≤ 5%.

### Calibrated Parameters

| Parameter | NPC Mode | SoC Mode | Rationale |
|-----------|----------|----------|-----------|
| `sdram_lat` | 45 | 55 | SDRAM first-beat (includes PMemBox/XBar overhead) |
| `sdram_burst_lat` | 10 | 23 | Per subsequent beat |
| `sram_lat` | — | 1 | On-chip SRAM (1 cycle) |

NPC mode models a simple PMemBox (DPI-C latency 40/8 + 2-cycle FSM
overhead ≈ 42/10, rounded to 45/10). SoC mode models the full
ysyxSoC XBar + SDRAM controller path.

### Current Calibration Results

**NPC CoreMark** (12/12 configs, ≤3.1% error):

| iCache Size | 8B line | 16B line | 32B line |
|-------------|---------|----------|----------|
| 256B | +0.27% | -2.13% | +0.49% |
| 512B | +3.07% | +0.21% | +0.93% |
| 1kB | -0.44% | +0.45% | +0.51% |
| 4kB | +0.52% | +0.56% | +0.58% |

**SoC CoreMark** (12/12 configs, ≤4.9% error):

| iCache Size | 8B line | 16B line | 32B line |
|-------------|---------|----------|----------|
| 256B | -1.19% | -2.44% | -4.92% |
| 512B | +0.21% | +0.11% | -1.39% |
| 1kB | -0.68% | -0.05% | -0.85% |
| 4kB | +0.77% | +0.49% | -0.02% |

**MicroBench (train):**
NPC: 512B/16B +13.8% (known outlier), others ≤2.3%.
SoC: all ≤5.1%.
