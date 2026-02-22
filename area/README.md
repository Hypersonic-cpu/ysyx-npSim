# Area Estimation

Chip area estimation for npSim configurations using
[CACTI 7.0](https://github.com/HewlettPackard/cacti) (NanGate 45nm)
and analytical DFF models.

## Quick Start

```bash
# From a timing simulation output (has config in JSON):
bash area/est-json.sh simout/my_run/conf.json

# Or with --dry-run (no trace needed):
./build/npsim.elf --dry-run --outdir my_config \
    --l1i-size 1kB --l1i-blksize 16 --l1i-assoc 1 \
    --bpu-type bimodal --bpu-size 16 --btb-size 16
bash area/est-json.sh simout/my_config/conf.json
```

Output goes to `areaout/<mirror_of_simout_path>/area_comp.json`.

## How It Works

Each `SimObject` in npSim exposes an `"area"` field in its `config_json()`.
This field has three parts:

- **`timing_area`** — known area from DFF-based structures (e.g., pipeline
  registers), in um².
- **`comb_percent`** — fraction of total area that is combinational logic
  (used to scale up: `total = (timing + sram) / (1 - comb_percent)`).
- **`cacti_objs`** — list of SRAM-like structures to estimate via CACTI.
  Each object specifies `type` (cache or ram), `size`, `block_size`/`word_size`,
  and `assoc`.

The Python script `area_est.py` reads the config JSON, generates CACTI
input files for each SRAM object, runs CACTI, and aggregates the results.

## Estimation Method per Component

| Component | Method | Details |
|-----------|--------|---------|
| Pipeline (Core) | Fixed | 14000 um² from RTL synthesis (comb_percent=0) |
| iCache / dCache | CACTI cache | Data + tag arrays. comb_percent=0.15 |
| StoreBuffer | DFF | entries × 72 bits × 5 um²/bit. comb_percent=0.3 |
| BimodalBP | CACTI ram or DFF | Counter table (entries × 1 byte) |
| GShareBP | CACTI ram or DFF | Counter table + shift register |
| TournamentBP | CACTI ram or DFF | Three tables (meta + local + global) |
| CompressedBTB | CACTI ram or DFF | entries × 9 bytes |
| BranchUnit | Fixed | 500 um² combinational wrapper |
| RAMArbiter (SDRAM) | Fixed | 200 um² combinational arbiter |
| Prefetchers | Fixed | 0 um² (combinational only, negligible) |

## CACTI Fallback Chain

For each SRAM object, the script tries three methods in order:

1. **CACTI cache mode** — generates a `.cfg` with the exact cache parameters
   (size, block size, associativity). This is the most accurate but CACTI
   requires roughly ≥32 cache lines to succeed. Only attempted for
   `type: "cache"` objects.

2. **CACTI ram mode** — models the structure as a scratch RAM. If the original
   word size gives too few entries for CACTI (< 16), the script tries
   progressively smaller word sizes (block/2, block/4, 8B, 4B) until CACTI
   succeeds. This loses tag-array modeling but gives reasonable total SRAM area.

3. **DFF estimate** — last resort for structures smaller than 128 bytes or
   when both CACTI modes fail. Uses 5 um²/bit, which is accurate for
   flip-flop-based register files but overestimates for SRAM. Values marked
   with `(*)` in plots use this method.

### When does each method get used?

CACTI needs a minimum number of entries (~16-32) to produce valid results.
The key factor is `entries = size / block_size`:

| Cache Config | Entries | Method |
|-------------|---------|--------|
| 4kB / 64B line | 64 | CACTI cache |
| 4kB / 32B line | 128 | CACTI cache |
| 1kB / 16B line | 64 | CACTI cache |
| 1kB / 32B line | 32 | CACTI cache or ram |
| 512B / 32B line | 16 | Ram (reduced word) |
| 256B / 16B line | 16 | Ram (reduced word) |
| BTB 16 entries × 9B = 144B | 16-36 | Ram (word=4B or 8B) |
| BPU 16 entries × 1B = 16B | < 128B | DFF |

Structures below `CACTI_MIN_BYTES` (128B) skip CACTI entirely and use DFF.

## Output Format

`area_comp.json` contains:

```json
{
  "total_area_um2": 25938.0,
  "total_area_mm2": 0.025938,
  "components": [
    {
      "name": "iCache",
      "total_um2": 8944.0,
      "timing_area_um2": 0.0,
      "sram_area_um2": 7602.7,
      "comb_area_um2": 1341.3,
      "sram_details": {
        "sram": {
          "data_um2": 6094.4,
          "tag_um2": 1508.3,
          "total_um2": 7602.7
        }
      }
    }
  ]
}
```

When ram fallback is used, `sram_details` includes `"mode": "ram_fallback"`.
When DFF is used, it includes `"dff_fallback": true`.

## Files

- `area_est.py` — main estimation script (run via `est-json.sh`)
- `est-json.sh` — shell wrapper that maps simout paths to areaout paths
- `../libs/cacti/` — CACTI 7.0 submodule (requires build, see below)
- `../src/areaSim/AreaEst.hh` — C++ helpers for `config_json()` area fields

## Building CACTI

```bash
cd libs/cacti
make -f cacti.mk clean && make -f cacti.mk -j4
```

On aarch64, first remove x86 flags:
```bash
sed -i 's/-g  -msse2 -mfpmath=sse -DNTHREADS/-g -O2 -DNTHREADS/' cacti.mk
sed -i 's/g++ -m64/g++/' cacti.mk
sed -i 's/gcc -m64/gcc/' cacti.mk
```
