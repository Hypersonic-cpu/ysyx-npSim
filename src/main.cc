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
#include "debug.hh"
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
static tint_t mem_bstlat = 8;
static std::string trace_file;
static size_t l1i_size = 32 * 1024;
static size_t l1i_blksize = 64;
static size_t l1i_assoc = 8;
static size_t max_insts = 0;
static std::string out_file;
static std::vector<SimObject*> simlist{};

// Dummy pmem_read for CacheSimulator
tint_t
pmem_read(addr_t addr, addr_t* ret, bool bfirst) {
  // if (ret) *ret = 0;
  return bfirst ? mem_latency : mem_bstlat;
}

tint_t
pmem_write(addr_t addr, word_t data, unsigned char mask, bool bfirst) {
  return bfirst ? mem_latency : mem_bstlat;
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
    {"max-insts", required_argument, 0, 'n'},
    {"debug-flags", required_argument, 0, 'd'},
    {"mem-lat", required_argument, 0, 'M'},
    {"mem-bstlat", required_argument, 0, 'm'},
    {"outfile", required_argument, 0, 'O'},
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
      out_file = optarg;
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

  // Write to file immediately
  if (!out_file.empty()) {
    std::string dir = "./simout";
    std::string path = dir + "/" + out_file;
    std::ofstream ofs(path);
    if (!ofs) {
      std::cerr << "Cannot open output file for writing: " << path << "\n";
    } else {
      ofs << root.dump(4) << "\n";
      ofs.close();
      std::cout << "Wrote stats JSON to " << path << "\n";
    }
  }
}

int
main(int argc, char** argv) {
  if (auto retcode = parse_args(argc, argv)) {
    return retcode;
  }

  // Ensure output directory and file exist immediately
  if (!out_file.empty()) {
    std::string dir = "./simout";
    std::string path = dir + "/" + out_file;
    std::string cmd = "mkdir -p " + dir;
    [[maybe_unused]]
    int ret = system(cmd.c_str());

    std::ofstream ofs(path);
    if (!ofs) {
      std::cerr << "Error: Cannot create output file: " << path
                << ". Aborting.\n";
      return 1;
    }
    ofs.close();
  }

  TraceReader reader(trace_file.c_str());
  Pipeline pipe(3, 0, 2);
  BimodalPredictor bpu(12); // Default 4K entries (2^12)
  CacheSimulator icache(l1i_size, l1i_blksize, l1i_assoc);
  simlist.push_back(std::addressof(pipe));
  simlist.push_back(std::addressof(icache));
  simlist.push_back(std::addressof(bpu));

  TraceInst inst;
  word_t dummy_word;

  // Root JSON object
  json root;
  // Add config once at the beginning
  root["config"] = collect_config_json(simlist);
  int dump_cnt = 0;

  // Default data latency
  tint_t load_lat = mem_latency;
  tint_t store_lat = mem_latency;

  while (reader.next(inst)) {
    if (max_insts > 0 && pipe.stats.insts >= max_insts)
      break;

    // BPU Predict
    bool pred_taken = false;
    // TODO: 每个周期都预测嘛？
    // if (inst.is_branch) {
    //   pred_taken = bpu.predict(inst.pc);
    // }

    bool real_taken = (inst.br_taken != 0);
    bool mispred = false;
    if (inst.is_branch) {
      mispred = (pred_taken != real_taken);
      bpu.update(inst.pc, real_taken);
    }

    g_tick = pipe.get_total_cycles();
    tint_t fetch_lat = icache.read_req(inst.pc, &dummy_word);

    pipe.iota_inst(inst, fetch_lat, load_lat, store_lat, mispred);

    if (inst.sys_op == SysOp::SysResetStats) [[unlikely]] {
      std::println(ANSI_FG_YELLOW
                   "Reset Stats @ PC 0x{:8x} Cyc #{:d}" ANSI_NONE,
                   inst.pc, pipe.get_total_cycles());
      for (auto* obj : simlist) {
        obj->reset_stats();
      }
    } else if (inst.sys_op == SysOp::SysDumpStats) [[unlikely]] {
      std::println(ANSI_FG_YELLOW
                   "Dump Stats @ PC 0x{:8x} Cyc #{:d}" ANSI_NONE,
                   inst.pc, pipe.get_total_cycles());
      for (const auto* obj : simlist) {
        obj->dump_stats();
      }
      append_stats_json(root, dump_cnt++);
    }
  }
  return 0;
}
