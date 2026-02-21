# npSim — Trace-Driven RISC-V Microarchitecture Simulator

npSim replays instruction traces captured from NEMU through a configurable
pipeline, cache hierarchy, branch predictor, and SDRAM model. It produces
cycle-approximate performance metrics (IPC, miss rates, stall breakdowns)
that track the RTL implementation within a few percent.

Traces are 16-byte packed `TraceInst` records (optionally zstd-compressed)
containing PC, memory address, register indices, branch outcome, and a
system-op field for resetting/dumping stats mid-trace.


## Building

Requires **clang++-22** with **libc++** and C++23 support.

```bash
# Release build (for sweeps and benchmarking)
make all -j4 DEBUG_MODE=0 NPSIM_ACTIVE=1

# Debug build (enables DPRINTF/DPRINTFS macros)
make all -j4 DEBUG_MODE=1 NPSIM_ACTIVE=1
```

The binary is placed at `build/npsim.elf`.


## Running

```bash
./build/npsim.elf <trace_file> [options]
```

Common options:

| Flag | Default | Description |
|------|---------|-------------|
| `--l1i-size` | `1kB` | iCache total size |
| `--l1i-blksize` | `16` | iCache line size in bytes |
| `--l1i-assoc` | `1` | iCache associativity |
| `--l1d-size` | `0` | dCache size (0 = StoreBuffer mode) |
| `--stbuf-entries` | `2` | StoreBuffer FIFO depth |
| `--bpu-type` | (none) | Branch predictor: `bimodal`, `gshare`, `tournament`, `alwaystaken`, `btfnt` |
| `--bpu-size` | `16` | BPU table entries (power of 2) |
| `--btb-size` | `16` | BTB entries (power of 2) |
| `--ipf-type` | `none` | iCache prefetcher: `nextline`, `stride`, `tagged` |
| `--mem-lat` | `42` | SDRAM first-beat latency |
| `--mem-bstlat` | `10` | SDRAM per-beat burst latency |
| `--outfile` | (none) | JSON stats output path |
| `--max-ticks` | ∞ | Stop simulation after N ticks |
| `--debug-flags` | (none) | Comma-separated debug flags |

Example:

```bash
./build/npsim.elf tests/coremark-10rnd-vld.nptr.zst \
  --l1i-size 1kB --l1i-assoc 2 --l1i-blksize 32 \
  --bpu-type bimodal --bpu-size 64 --btb-size 32 \
  --outfile run.json
```


## Generating Traces

Traces are captured from NEMU with CONFIG_NPSIM_TRACE enabled:

```bash
# Build benchmark (mainargs baked at compile time)
cd $AM_HOME/../benchmarks/coremark
make ARCH=riscv32e-nemu mainargs= insert-arg

# Run NEMU in batch mode, writing trace to file
$NEMU_HOME/build/riscv32-nemu-interpreter -b \
  --nptr=tests/coremark-10rnd-vld.nptr.zst \
  build/coremark-riscv32e-nemu.bin
```

For microbench, pass the size via `mainargs=test` or `mainargs=train`, then
call `make ... insert-arg` to embed it in the binary before running NEMU.


## Module Layout

```
src/
├── main.cc                  Simulation setup, CLI, trace feed loop
├── trace.{cc,hh}            TraceInst struct, zstd/xz reader
├── pipeSim/
│   ├── Pipeline.hh          5-stage in-order pipeline definition
│   └── Pipeline.cc          Fetch, decode, execute, memory, writeback
├── cacheSim/
│   ├── CacheBase.{hh,cc}    PipeCache, NoCache, StoreBuffer
│   ├── CacheLine.{hh,cc}    Cache line with tag, valid, dirty bits
│   ├── Prefetcher.hh        NextLine, Stride, Tagged prefetchers
│   └── RamConn.{hh,cc}      RAMArbiter (single-channel SDRAM model)
├── branchSim/
│   ├── BranchPred.hh        BranchUnit, Bimodal, GShare, Tournament, ...
│   └── BranchPred.cc        Predictor implementations
└── defines/
    ├── base.hh              SimObject / ClockedObject base classes
    ├── types.hh             addr_t, word_t, tick_t
    ├── interface.hh         CpuTrans, AckTrans, MemTrans definitions
    └── debug.{hh,cc}        Debug flag infrastructure
```


## Simulation Architecture

The main loop advances a global tick counter. Each tick, all devices are
updated bottom-up: SDRAM → dCache → iCache → Pipeline. The pipeline
requests the next trace instruction via `feed_inst()` when ready.

The pipeline is a 5-stage in-order design (Fetch → Decode → Execute →
Memory → WriteBack) that tracks RAW hazards via per-register ready times.
Branch prediction happens at fetch; mispredictions inject penalty fetches
and stall the IFU.

The SDRAM model is a single-channel arbiter shared by iCache and dCache,
with separate read/write channels. Higher-indexed hosts have priority
(dCache > iCache). Latency = `mem_latency + (burst_len - 1) * mem_bstlat`.


## Stats Output

JSON output has the structure `{ "config": {...}, "stats0": {...}, "stats1": {...} }`.
Stats windows are delimited by `SysResetStats` / `SysDumpStats` trace ops
embedded by the benchmark harness. The key metric is `stats0.Core.ipc`.


## Design-Space Exploration

Sweep scripts in `scripts/` run the simulator across parameter combinations:

```bash
bash scripts/sweep_a.sh    # cache size × line size × associativity
bash scripts/sweep_b.sh    # branch predictor × iCache prefetcher
bash scripts/sweep_c.sh    # StoreBuffer vs dCache × data prefetcher
```

Results land in `simout/sweep_{a,b,c}/`. Generate plots with:

```bash
python3 visual/plot_sweeps.py
```

Plots are saved to `visual/plots/`.


## RTL Validation

Baseline IPC alignment with RTL (default config, no BPU, no prefetcher):

```
             RTL IPC    npSim IPC    Error
CoreMark     0.1363     0.1374      +0.8%
MicroTrain   0.1491     0.1473      -1.2%
```

Sweep configs (line size 8/32, StoreBuffer 4/8) stay within 5%.
