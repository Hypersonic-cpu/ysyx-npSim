#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <limits>
#include <memory>
#include <print>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

#include "branchSim/BranchPred.hh"
#include "cacheSim/CacheBase.hh"
#include "cacheSim/Prefetcher.hh"
#include "cacheSim/RamConn.hh"
#include "defines/base.hh"

#include "defines/debug.hh"
#include "defines/interface.hh"
#include "defines/types.hh"
#include "pipeSim/Pipeline.hh"
#include "trace.hh"

#include "nlohmann/json.hpp"
#include "stats.hpp"

using namespace trace;
using namespace debug;
using branchSim::BranchUnit;
using branchSim::BTBBase;
using cacheSim::CacheBase;
using memSim::RAMArbiter;

// Global simulation tick counter
static tick_t g_tick = 0;

tick_t
curr_tick() noexcept {
  return g_tick;
}

void
set_global_tick(tick_t t) noexcept {
  DPRINTFS(Clock, " == Global Tick Fwd @ %lu -> %lu ==", g_tick, t);
  assert(t >= g_tick);
  g_tick = t;
}

// Memory latency parameters (microsecond-based, converted to cycles
// via freq_mhz: cycles = ceil(lat_us * freq_mhz))
//   NPC mode: fixed SDRAM latency via PMemBox (DPI-C ~40ns + overhead)
//   SoC mode: SdramModel (bank-aware row-hit/miss/conflict)
static double sdram_lat_us = 0.043;           // NPC default: 43ns = 0.043us
static double sdram_burst_us = 0.016;         // NPC default: 16ns = 0.016us
static tint_t axi_ovhd_cyc = 0;              // Fixed AXI protocol overhead (cycles)
static tint_t sram_lat = 1;                  // SoC: on-chip SRAM latency (cycles)
// Derived (set by parse_args from sdram_*_us x freq_mhz)
static tint_t sdram_lat_cyc = 0;
static tint_t sdram_burst_cyc = 0;
static std::string trace_file;
// RTL: iCacheConf(32, 1024, 16, 1) -- 1KB, 16B line, direct-mapped
static size_t l1i_size = 1024;
static size_t l1i_blksize = 16;
static size_t l1i_assoc = 1;
static size_t l1d_size = 0;
static size_t l1d_blksize = 16;
static size_t l1d_assoc = 1;
static size_t max_insts = std::numeric_limits<size_t>::max();
static size_t max_ticks = std::numeric_limits<tick_t>::max();
static std::string out_dir;
static bool dry_run = false;

// BPU Config
static std::string bpu_type = "";
static size_t bpu_entries_pow2 = 4; // 16
static size_t btb_entries_pow2 = 4;
static size_t ras_depth = 0;
static uint8_t print_mode = 2;

// Pipeline Queue sizes
static size_t ifq_size = 4; // RTL FetchStage PipeDepth+1
// FIXME: Remove this. NoCache means no buffer
static size_t stq_size = 8; // Only used when dCache is NoCache
static size_t stbuf_entries = 0;
static tick_t br_mis_pen = 1; // Branch misprediction penalty (cycles)
static tick_t mmio_lat = 3;  // MMIO access latency (cycles, SoC only)
static int freq_mhz = 1000;  // CPU frequency in MHz (default 1 GHz)
static std::string l1i_pref_type = "none"; // iCache prefetcher type
static std::string l1d_pref_type = "none"; // dCache prefetcher type
static bool sram_dff = true;          // Area model: DFF or SRAM macro

// Dummy physical memory stubs (active mode: caches don't read data)
void
pmem_read(addr_t addr, addr_t* ret) {}
void
pmem_write(addr_t addr, word_t data, unsigned char mask) {}

size_t
parse_size(const std::string& s) {
  size_t mult = 1;
  std::string num = s;
  if (s.ends_with("kB") || s.ends_with("KB")) {
    mult = 1024;
    num = s.substr(0, s.size() - 2);
  } else if (s.ends_with("MB")) {
    mult = 1024 * 1024;
    num = s.substr(0, s.size() - 2);
  } else if (s.ends_with("B")) {
    num = s.substr(0, s.size() - 1);
  }
  return std::stoul(num) * mult;
}

inline int
parse_args(int argc, char* argv[]) {
  static struct option long_options[] = {
    {"l1i-size", required_argument, 0, 's'},
    {"l1i-blksize", required_argument, 0, 'b'},
    {"l1i-assoc", required_argument, 0, 'a'},
    {"l1d-size", required_argument, 0, 'S'},
    {"l1d-blksize", required_argument, 0, 'B'},
    {"l1d-assoc", required_argument, 0, 'A'},
    {"max-insts", required_argument, 0, 'n'},
    {"max-ticks", required_argument, 0, 'N'},
    {"debug-flags", required_argument, 0, 'd'},
    {"sdram-lat-us", required_argument, 0, 'M'},
    {"sdram-burst-us", required_argument, 0, 'm'},
    {"outdir", required_argument, 0, 'O'},
    {"dry-run", no_argument, 0, 'D'},
    {"bpu-type", required_argument, 0, 'T'},
    {"bpu-size", required_argument, 0, 'e'},
    {"btb-size", required_argument, 0, 't'},
    {"ras-size", required_argument, 0, 'R'},
    {"ifq-size", required_argument, 0, 'q'},
    {"stbuf-entries", required_argument, 0, 'Z'},
    {"br-pen", required_argument, 0, 'X'},
    {"l1i-pref", required_argument, 0, 'P'},
    {"l1d-pref", required_argument, 0, 'p'},
    {"print-brief", no_argument, 0, 201U},
    {"print-none", no_argument, 0, 200U},
    {"npc-mode", no_argument, 0, 202U},
    {"sram-lat", required_argument, 0, 203U},
    {"sram-dff", no_argument, 0, 204U},
    {"sram-lib", no_argument, 0, 205U},
    {"mmio-lat", required_argument, 0, 206U},
    {"freq-mhz", required_argument, 0, 207U},
    {"axi-ovhd-cyc", required_argument, 0, 208U},
    {0, 0, 0, 0}};

  int opt;
  int option_index = 0;
  while ((opt = getopt_long(argc, argv, "O:", long_options, &option_index))
         != -1) {
    switch (opt) {
    case 's':
      l1i_size = parse_size(optarg);
      break;
    case 'b':
      l1i_blksize = parse_size(optarg);
      break;
    case 'a':
      l1i_assoc = parse_size(optarg);
      break;
    case 'S':
      l1d_size = parse_size(optarg);
      break;
    case 'B':
      l1d_blksize = parse_size(optarg);
      break;
    case 'A':
      l1d_assoc = parse_size(optarg);
      break;
    case 'n':
      max_insts = std::stoul(optarg);
      break;
    case 'N':
      max_ticks = std::stoul(optarg);
      break;
    case 'd':
      debug::set_flags(optarg);
      break;
    case 'M':
      sdram_lat_us = std::stod(optarg);
      break;
    case 'm':
      sdram_burst_us = std::stod(optarg);
      break;
    case 'O':
      out_dir = optarg;
      break;
    case 'D':
      dry_run = true;
      break;
    case 'T':
      bpu_type = optarg;
      break;
    case 'e':
      bpu_entries_pow2 =
        std::log2(static_cast<double>(std::stoul(optarg)) + 0.5);
      break;
    case 't':
      btb_entries_pow2 =
        std::log2(static_cast<double>(std::stoul(optarg)) + 0.5);
      break;
    case 'R':
      ras_depth = std::stoul(optarg);
      break;
    case 'q':
      ifq_size = std::stoul(optarg);
      break;
    case 'Z':
      stbuf_entries = std::stoul(optarg);
      break;
    case 'X':
      br_mis_pen = std::stoul(optarg);
      break;
    case 'P':
      l1i_pref_type = optarg;
      break;
    case 'p':
      l1d_pref_type = optarg;
      break;
    case 201:
      print_mode = 1;
      break;
    case 200:
      print_mode = 0;
      break;
    case 202:
      g_soc_mode = false;
      break;
    case 203:
      sram_lat = std::stoul(optarg);
      break;
    case 204:
      sram_dff = true;
      break;
    case 205:
      sram_dff = false;
      break;
    case 206:
      mmio_lat = std::stoul(optarg);
      break;
    case 207:
      freq_mhz = std::stoi(optarg);
      break;
    case 208:
      axi_ovhd_cyc = std::stoul(optarg);
      break;
    default:
      std::cerr << "Usage: " << argv[0] << " <trace_file> [options]\n";
      return 1;
    }
  }

  if (optind < argc) {
    trace_file = argv[optind];
  } else if (!dry_run) {
    std::cerr << "Usage: " << argv[0] << " <trace_file> [options]\n"
              << "  --dry-run   Dump config/area without simulation\n"
              << "  --outdir=DIR  Output to simout/DIR/{conf,stats}.json\n";
    return 1;
  }

  // Convert microsecond latencies to cycle counts.
  //   cycles = ceil(lat_us * freq_mhz)
  // At 1 GHz: 0.043 us x 1000 = 43 cycles.
  auto us_to_cyc = [](double us, int mhz) -> tint_t {
    return std::max<tint_t>(
      1, static_cast<tint_t>(std::ceil(us * mhz)));
  };
  sdram_lat_cyc = us_to_cyc(sdram_lat_us, freq_mhz);
  sdram_burst_cyc = us_to_cyc(sdram_burst_us, freq_mhz);

  return 0;
}

std::unique_ptr<branchSim::BranchPred>
create_bpu_core() {
  if (bpu_type == "bimodal") {
    return std::make_unique<branchSim::BimodalPredictor>("BimodalBP",
                                                         bpu_entries_pow2);
  } else if (bpu_type == "gshare") {
    return std::make_unique<branchSim::GSharePredictor>(
      "GShareBP", bpu_entries_pow2,
      12); // 12-bit history
  } else if (bpu_type == "tournament") {
    return std::make_unique<branchSim::TournamentPredictor>(
      "TournamentBP", bpu_entries_pow2, 12); // 12-bit history
  } else if (bpu_type == "alwaystaken") {
    return std::make_unique<branchSim::AlwaysTakenPredictor>();
  } else if (bpu_type == "btfnt") {
    return std::make_unique<branchSim::BTFNTPredictor>();
  } else if (bpu_type == "none" || bpu_type.empty()) {
    return std::make_unique<branchSim::NoBPU>();
  } else {
    assert(false && "No such branch predictor");
    return nullptr;
  }
}

std::unique_ptr<BTBBase>
create_btb() {
  if (btb_entries_pow2 == 0) {
    return std::make_unique<branchSim::NoBTB>();
  }
  return std::make_unique<branchSim::CompressedBTB>(
    "BTB", btb_entries_pow2, 10, 20, sram_dff);
}

std::unique_ptr<BranchUnit>
create_branch_unit() {
  auto bpu = create_bpu_core();
  auto btb = create_btb();
  return std::make_unique<BranchUnit>(std::move(bpu), std::move(btb), ras_depth);
}

using json = nlohmann::ordered_json;

inline json
collect_stats_json(const std::vector<SimObject*>& simlist) {
  json stats_obj;
  for (const auto* obj : simlist) {
    stats_obj[obj->name()] = obj->stats_json();
  }
  return stats_obj;
}

inline json
collect_config_json(const std::vector<SimObject*>& simlist) {
  json config_obj;
  for (const auto* obj : simlist) {
    config_obj[obj->name()] = obj->config_json();
  }
  return config_obj;
}

inline void
append_stats_json(json& root, size_t curr_cnt,
                  const std::vector<SimObject*>& simlist,
                  const TraceSanitizer& san) {
  std::string key = "stats" + std::to_string(curr_cnt);
  json stats_obj = collect_stats_json(simlist);
  stats_obj["TraceSanitizer"] = {
    {"total", san.total},
    {"loads", san.loads},
    {"stores", san.stores},
    {"branches", san.branches},
    {"br_taken", san.br_taken},
    {"br_not_taken", san.br_not_taken},
    {"alu", san.alu},
    {"has_dst", san.has_dst},
    {"has_src1", san.has_src1},
    {"has_src2", san.has_src2},
    {"sys_ops", san.sys_ops},
    {"rvm_ops", san.m_ext_ops},
    {"br_taken_no_target", san.br_taken_no_target},
  };
  root[key] = stats_obj;
}

inline void
outdir_ensure(const std::string& dir) {
  if (!dir.empty()) {
    std::string cmd = "mkdir -p simout/" + dir;
    [[maybe_unused]] int ret = system(cmd.c_str());
    assert(!ret && "Cannot create output directory");
  }
}

inline void
outfile_write(const std::string& path, const json& root) {
  std::ofstream ofs(path);
  if (!ofs) {
    std::cerr << "Cannot open output file for writing: " << path << "\n";
  } else {
    ofs << root.dump(4) << "\n";
    ofs.close();
    std::cout << "Wrote JSON to " << path << "\n";
  }
}

int
main(int argc, char** argv) {
  if (auto retcode = parse_args(argc, argv)) {
    return retcode;
  }

  outdir_ensure(out_dir);

  /** Component Configuration */
  auto branch_unit = create_branch_unit();

  // When dCache exists, no need for store queue (write-through)
  // Only use store queue when NoCache (need buffering for SDRAM)
  // FIXME:
  size_t actual_stq_size = (l1d_size > 0) ? 0 : stq_size;
  auto core = std::make_unique<pipeSim::Pipeline>(
    "Core", ifq_size, actual_stq_size, branch_unit.get(),
    br_mis_pen, mmio_lat);

  std::shared_ptr<cacheSim::Prefetcher> ipf = nullptr;
  if (l1i_pref_type == "nextline") {
    ipf = std::make_shared<cacheSim::NextLinePrefetcher>("iCache");
  } else if (l1i_pref_type == "stride") {
    ipf = std::make_shared<cacheSim::StridePrefetcher>("iCache");
  } else if (l1i_pref_type == "tagged") {
    ipf = std::make_shared<cacheSim::TaggedPrefetcher>("iCache");
  }

  auto icache = std::make_unique<cacheSim::PipeCache>(
    "iCache",
    /* host */ core.get(),
    /* pipe depth */ 3, l1i_size, l1i_blksize, l1i_assoc, ipf,
    /* cache ID */ 0, sram_dff);
  std::unique_ptr<cacheSim::CacheBase> dcache = nullptr;
  if (l1d_size > 0) {
    std::shared_ptr<cacheSim::Prefetcher> dpf = nullptr;
    if (l1d_pref_type == "stride") {
      dpf = std::make_shared<cacheSim::StridePrefetcher>("dCache");
    } else if (l1d_pref_type == "nextline") {
      dpf = std::make_shared<cacheSim::NextLinePrefetcher>("dCache");
    } else if (l1d_pref_type == "tagged") {
      dpf = std::make_shared<cacheSim::TaggedPrefetcher>("dCache");
    }
    dcache = std::make_unique<cacheSim::PipeCache>(
      "dCache",
      /* host */ core.get(),
      /* pipe depth */ 1, l1d_size, l1d_blksize, l1d_assoc, dpf,
      /* cache ID */ 1, sram_dff,
      /* write_back */ true);
  } else {
    if (stbuf_entries == 0) {
      dcache = std::make_unique<cacheSim::NoCache>("dNoCache",
                                                   static_cast<uint16_t>(1));
    } else {
      dcache = std::make_unique<cacheSim::StoreBuffer>(
        "stBuf", stbuf_entries, static_cast<uint16_t>(1));
    }
  }
  core->set_cache_ports(icache.get(), dcache.get());
  pipeSim::Processor* proc = &(*core);

  // SoC mode: bank-aware SDRAM timing model.
  // NPC mode: fixed-latency (sdram_lat_cyc / sdram_burst_cyc).
  std::unique_ptr<memSim::SdramModel> sdram_model;
  if (g_soc_mode) {
    sdram_model =
        std::make_unique<memSim::SdramModel>(freq_mhz);
  }

  auto sdram = std::make_unique<memSim::RAMArbiter>(
    "SDRAM", sdram_lat_cyc, sdram_burst_cyc,
    std::vector<CacheBase*>({icache.get(), dcache.get()}), sram_lat,
    axi_ovhd_cyc, sdram_model.get());
  icache->set_mem_port(sdram.get());
  dcache->set_mem_port(sdram.get());
  CpuSideAckReceiver cpu_ack = [proc](auto t) { proc->ack_mem_avail(t); };
  CpuSideMRespReceiver cpu_rsp = [proc](auto p) { proc->recv_mem_resp(p); };
  icache->set_cpu_side_handlers(cpu_rsp, cpu_ack);
  dcache->set_cpu_side_handlers(cpu_rsp, cpu_ack);

  std::vector<SimObject*> simlist{sdram.get(), dcache.get(), icache.get(),
                                  branch_unit.get(), core.get()};
  if (ipf)
    simlist.push_back(ipf.get());

  // Dump config (shared by --dry-run and normal mode)
  json root;
  root["config"] = collect_config_json(simlist);

  if (!out_dir.empty()) {
    outfile_write("simout/" + out_dir + "/conf.json", root);
  }

  // --dry-run: dump config only, skip simulation
  if (dry_run) {
    if (out_dir.empty()) {
      std::cout << root.dump(4) << "\n";
    }
    return 0;
  }

  TraceReader reader(trace_file.c_str());
  TraceSanitizer sanitizer;     // windowed (resets with SysResetStats)
  TraceSanitizer sanitizer_all; // cumulative (never resets)

  // NOTE: Bottom-up order. Mem -> Cache -> CPU
  const std::vector<ClockedObject*> devlist{sdram.get(), dcache.get(),
                                            icache.get(), core.get()};

  TraceInst inst;
  bool has_next = true;

  size_t dump_cnt = 0;
  size_t inst_cnt = 0;

  auto loop_start = std::chrono::high_resolution_clock::now();
  // Main SimLoop
  do {
    for (auto dev : devlist) {
      dev->do_update();
    }

    if (core->inst_avail()) {
      if (has_next && inst_cnt < max_insts) [[likely]] {
        has_next = reader.next(inst);
        if (!has_next) {
          core->set_draining();
        } else {
          core->feed_inst(inst);
          sanitizer.record(inst);
          sanitizer_all.record(inst);
          inst_cnt++;
          DPRINTFS(
            Main, "Inst feed: PC %8x rs%2d:%2d rd%2d mem%1d:%8x br%1d:%1d",
            inst.pc, inst.src_reg[0], inst.src_reg[1], inst.dst_reg,
            inst.mem_op, inst.mem_addr, inst.is_branch, inst.br_taken);
        }

      } else {
        core->set_draining();
      }
    }

    set_global_tick(curr_tick() + 1);

    if (inst.sys_op == SysOp::SysResetStats) [[unlikely]] {
      inst.sys_op = SysOp::SysNone;
      std::println(ANSI_FG_YELLOW
                   "Reset Stats @ PC 0x{:8x} Cyc #{:d}" ANSI_ALL_NONE,
                   inst.pc, core->stats.cycles);
      for (auto* obj : simlist) {
        obj->reset_stats();
      }
      sanitizer.reset();
    } else if (inst.sys_op == SysOp::SysDumpStats) [[unlikely]] {
      inst.sys_op = SysOp::SysNone;
      std::println(ANSI_FG_YELLOW
                   "Dump Stats @ PC 0x{:8x} Cyc #{:d}" ANSI_ALL_NONE,
                   inst.pc, core->stats.cycles);
      if (print_mode == 2) {
        for (const auto* obj : simlist) {
          obj->dump_stats();
        }
        sanitizer.dump();
      } else if (print_mode == 1) {
        std::println(
          "#Cyc {:d} IPC {:.6f} BPMR {:.6f} i$MR {:.6f} d$MR {:.6f}",
          core->stats.cycles, core->stats.get_ipc(),
          branch_unit->stats.miss_rate(), icache->stats.miss_rate(),
          dcache ? dcache->stats.miss_rate() : -1);
      }
      append_stats_json(root, dump_cnt++, simlist, sanitizer);
    }
  } while (!core->is_finished() && curr_tick() < max_ticks);

  auto loop_end = std::chrono::high_resolution_clock::now();
  auto loop_us = std::chrono::duration_cast<std::chrono::milliseconds>(
                   loop_end - loop_start)
                   .count();
  std::println(ANSI_FG_YELLOW
               "> Host Time: {:d} ms IPC: {:.6f} <" ANSI_ALL_NONE,
               loop_us, core->stats.get_ipc());

  // Dump final stats (use cumulative sanitizer)
  append_stats_json(root, dump_cnt++, simlist, sanitizer_all);
  if (print_mode == 2) {
    std::println("--- Cumulative Trace Summary ---");
    sanitizer_all.dump();
  }
  if (!out_dir.empty()) {
    outfile_write("simout/" + out_dir + "/stats.json", root);
  }
  return 0;
}
