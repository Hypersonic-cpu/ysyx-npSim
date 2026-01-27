#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include "base.hh"
#include "branchSim/BranchPredictor.hh"
#include "cacheSim/CacheSimulator.hh"
#include "cacheSim/Prefetcher.hh"
#include "debug.hh"
#include "nlohmann/detail/value_t.hpp"
#include "pipeSim/Pipeline.hh"
#include "stats.hh"
#include "trace.hh"
#include "types.hh"

using namespace trace;
using namespace pipeSim;
using namespace branchSim;
using namespace cacheSim;

// Global tick for CacheSimulator
static tick_t g_tick = 0;
tick_t
curr_tick() noexcept {
  return g_tick;
}

// Configuration parameters
static tint_t mem_latency = 40;
static tint_t mem_bstlat = 10;
static std::string trace_file;
// Tiny defaults
static size_t l1i_size = 512;
static size_t l1i_blksize = 16;
static size_t l1i_assoc = 1;
static size_t l1d_size = 0;
static size_t l1d_blksize = 16;
static size_t l1d_assoc = 1;
static std::string i_prefetch = "none";
static std::string d_prefetch = "none";
static size_t max_insts = 0;
static std::string out_file;
static std::vector<SimObject*> simlist{};

// BPU Config
static std::string bpu_type = "";
static size_t bpu_entries_pow2 = 4; // 16
static size_t btb_entries_pow2 = 4;
static bool use_ras = false;
static uint8_t print_mode = 2;

// IF Queue size
static size_t ifq_size = 3;

// Dummy pmem_read for CacheSimulator
// SDRAM use same wire for R/W
static tick_t loc_sdram_avail = 0;
tint_t
pmem_read(addr_t addr, addr_t* ret, bool bfirst) {
  //   auto wait =
  //     loc_sdram_avail > curr_tick() ? loc_sdram_avail - curr_tick() : 0;
  //   auto total = bfirst ? (mem_latency + wait) : mem_bstlat;
  //   loc_sdram_avail = curr_tick() + total;
  //   DPRINTF(Sdram, "SDRAM access @ %8x from %lu to %lu (wait %lu, total
  //   %lu)",
  //           addr, curr_tick(),
  //           loc_sdram_avail, wait, total);
  // return total;
  return bfirst ? mem_latency : mem_bstlat;
}

tint_t
pmem_write(addr_t addr, word_t data, unsigned char mask, bool bfirst) {
  return pmem_read(addr, nullptr, bfirst);
}

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
    {"l1i-pf", required_argument, 0, 'P'},
    {"l1d-pf", required_argument, 0, 'p'},
    {"max-insts", required_argument, 0, 'n'},
    {"debug-flags", required_argument, 0, 'd'},
    {"mem-lat", required_argument, 0, 'M'},
    {"mem-bstlat", required_argument, 0, 'm'},
    {"outfile", required_argument, 0, 'O'},
    {"bpu-type", required_argument, 0, 'T'},
    {"bpu-size", required_argument, 0, 'e'},
    {"btb-size", required_argument, 0, 't'},
    {"use-ras", no_argument, 0, 'R'},
    {"ifq-size", required_argument, 0, 'q'},
    {"print-brief", no_argument, 0, 201U},
    {"print-none", no_argument, 0, 200U},
    {0, 0, 0, 0}};

  int opt;
  int option_index = 0;
  while ((opt = getopt_long(argc, argv, "O:", long_options,
                            &option_index)) != -1) {
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
    case 'P':
      i_prefetch = optarg;
      break;
    case 'p':
      d_prefetch = optarg;
      break;
    case 'n':
      max_insts = std::stoul(optarg);
      break;
    case 'd':
      debug::set_flags(optarg);
      break;
    case 'M':
      mem_latency = std::stoul(optarg);
      break;
    case 'm':
      mem_bstlat = std::stoul(optarg);
      break;
    case 'O':
      out_file = std::string{"simout/"} + optarg;
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
      use_ras = true;
      break;
    case 'q':
      ifq_size = std::stoul(optarg);
      break;
    case 201:
      print_mode = 1;
      break;
    case 200:
      print_mode = 0;
      break;
    default:
      std::cerr << "Usage: " << argv[0] << " <trace_file> [options]\n";
      return 1;
    }
  }

  if (optind < argc) {
    trace_file = argv[optind];
  } else {
    std::cerr << "Usage: " << argv[0] << " <trace_file> [options]\n";
    return 1;
  }
  return 0;
}

std::shared_ptr<Prefetcher>
create_prefetcher(const std::string& type, const std::string& name) {
  if (type == "nextline")
    return std::make_shared<NextLinePrefetcher>(name);
  if (type == "stride")
    return std::make_shared<StridePrefetcher>(name);
  return nullptr;
}

std::shared_ptr<BranchPredictor>
create_bpu() {
  std::shared_ptr<BranchPredictor> bpu = nullptr;
  if (bpu_type == "bimodal") {
    bpu = std::make_shared<BimodalPredictor>("BinmodalBP", bpu_entries_pow2);
  } else if (bpu_type == "alwaystaken") {
    bpu = std::make_shared<AlwaysTakenPredictor>();
  } else if (bpu_type == "btfnt") {
    bpu = std::make_shared<BTFNTPredictor>();
  } else if (bpu_type == "none" || bpu_type.empty()) {
  } else {
    assert(false && "No such branch predictor");
  }

  if (use_ras) {
    assert(0 && "Unimplemented");
    // Wrap with RAS (16 entries default?)
    // bpu = std::make_shared<RASPredictorWrapper>(bpu, 16);
  }
  return bpu;
}

#include "nlohmann/json.hpp"

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
append_stats_json(json& root, size_t curr_cnt) {
  // Generate and store stats
  std::string key = "stats" + std::to_string(curr_cnt);
  root[key] = collect_stats_json(simlist);
}

inline void
outfile_write(const std::string& path, const json& root) {
  std::ofstream ofs(path);
  if (!ofs) {
    std::cerr << "Cannot open output file for writing: " << path << "\n";
  } else {
    ofs << root.dump(4) << "\n";
    ofs.close();
    std::cout << "Wrote stats JSON to " << path << "\n";
  }
}

inline void
outfile_check(const std::string& file) {
  if (!file.empty()) {
    std::string path = file;
    // Check if path has directory
    std::string dir = ".";
    if (path.find('/') != std::string::npos) {
      dir = path.substr(0, path.find_last_of('/'));
      std::string cmd = "mkdir -p " + dir;
      [[maybe_unused]] int ret = system(cmd.c_str());
      assert(!ret && "Cannot create output directory");
    }

    std::ofstream ofs(path);
    assert(ofs.is_open() && "Cannot create output file");
    ofs.close();
  }
}

int
main(int argc, char** argv) {
  if (auto retcode = parse_args(argc, argv)) {
    return retcode;
  }

  // Ensure output directory and file exist immediately
  outfile_check(out_file);

  TraceReader reader(trace_file.c_str());
  Pipeline pipe(ifq_size, 0, 2);

  /** Component Configuration */
  auto bpu = create_bpu();
  auto btb = std::make_shared<CompressedBTB>("BTB", btb_entries_pow2);

  auto iprefetcher = create_prefetcher(i_prefetch, "iPrefetcher");
  CacheSimulator icache("iCache", l1i_size, l1i_blksize, l1i_assoc,
                        iprefetcher);

  auto dprefetcher = create_prefetcher(d_prefetch, "dPrefetcher");
  std::unique_ptr<CacheSimulator> dcache = nullptr;
  if (l1d_size > 0) {
    dcache = std::make_unique<CacheSimulator>(
      "dCache", l1d_size, l1d_blksize, l1d_assoc, dprefetcher);
  }

  simlist.push_back(std::addressof(pipe));
  simlist.push_back(std::addressof(icache));
  if (dcache)
    simlist.push_back(dcache.get());
  if (bpu)
    simlist.push_back(bpu.get());
  simlist.push_back(btb.get());
  if (iprefetcher)
    simlist.push_back(iprefetcher.get());
  if (dprefetcher)
    simlist.push_back(dprefetcher.get());
  /** End of Configuration */

  TraceInst inst;
  word_t dummy_word;

  // Root JSON object
  json root;
  // Add config once at the beginning
  root["config"] = collect_config_json(simlist);

  int dump_cnt = 0;
  int inst_cnt = 0;
  // Main SimLoop
  while (reader.next(inst)) {
    if (max_insts > 0 && inst_cnt >= max_insts)
      break;
    inst_cnt++;

    // Branch Predict
    auto mispred = false;
    if (inst.is_branch) {
      auto btb_tar = btb->lookup(inst.pc);
      // When BTB miss, predict as not taken
      bool bpu_result = bpu ? bpu->predict(inst.pc, btb_tar) : false;
      bool pred_taken = bpu_result && btb_tar != 0;

      bool real_taken = (inst.br_taken != 0);

      if (real_taken) {
        btb->update(inst.pc, inst.mem_addr);
      }
      if (bpu) {
        mispred =
          !bpu->judge(real_taken, pred_taken, inst.mem_addr, btb_tar);
        bpu->update(inst.pc, real_taken);
      } else {
        mispred = real_taken;
      }
    }

    /** In event-driven simulator we use curr_tick(),
     * but in trace-driven, g_tick should be set back and forth
     * for different stage of a single instruction
     */
    g_tick = pipe.icache_access_time();
    tint_t fetch_lat = icache.read_req(inst.pc, &dummy_word);

    g_tick = pipe.load_store_time();
    tint_t load_lat = 0;
    tint_t store_lat = 0;
    // TODO: Set dcache size = 0 to disable
    if (inst.mem_op == MemOp::MemLoad) {
      if (l1d_size > 0)
        load_lat = dcache->read_req(inst.mem_addr, &dummy_word);
      else
        load_lat = pmem_read(inst.mem_addr, &dummy_word, true);
    } else if (inst.mem_op == MemOp::MemStore) {
      if (l1d_size > 0)
        store_lat = dcache->write_req(inst.mem_addr, 0, 0xF);
      else
        store_lat = pmem_write(inst.mem_addr, 0, 0xF, true);
    }
    // std::println("g_tick {:d} ld/st lat {:d} {:d}", g_tick, load_lat,
    // store_lat);

    pipe.iota_inst(inst, fetch_lat, load_lat, store_lat, mispred);

    if (inst.sys_op == SysOp::SysResetStats) [[unlikely]] {
      std::println(ANSI_FG_YELLOW
                   "Reset Stats @ PC 0x{:8x} Cyc #{:d}" ANSI_ALL_NONE,
                   inst.pc, pipe.stats.cycles);
      for (auto* obj : simlist) {
        obj->reset_stats();
      }
    } else if (inst.sys_op == SysOp::SysDumpStats) [[unlikely]] {
      std::println(ANSI_FG_YELLOW
                   "Dump Stats @ PC 0x{:8x} Cyc #{:d}" ANSI_ALL_NONE,
                   inst.pc, pipe.stats.cycles);
      if (print_mode == 2) {
        for (const auto* obj : simlist) {
          obj->dump_stats();
        }
      } else if (print_mode == 1) {
        std::println(
          "#Cyc {:d} IPC {:.6f} BPMR {:.6f} i$MR {:.6f} d$MR {:.6f}",
          pipe.stats.cycles, pipe.stats.get_ipc(),
          bpu ? bpu->stats.miss_rate() : -1, icache.stats.miss_rate(),
          dcache ? dcache->stats.miss_rate() : -1);
      }
      append_stats_json(root, dump_cnt++);
    }
  }

  // Dump final stats
  append_stats_json(root, dump_cnt++);
  outfile_write(out_file, root);
  return 0;
}
