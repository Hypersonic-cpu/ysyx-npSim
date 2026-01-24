#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>

#include "trace.hh"
#include "pipeSim/Pipeline.hh"
#include "branchSim/BranchPredictor.hh"
#include "cacheSim/CacheSimulator.hh"
#include "types.hh"
#include "base.hh"
#include "stats.hh"
#include "debug.hh"

using namespace trace;
using namespace pipeSim;
using namespace branchSim;
using namespace cacheSim;

// Global tick for CacheSimulator
static tick_t g_tick = 0;
tick_t curr_tick() noexcept { return g_tick; }

// Dummy pmem_read for CacheSimulator
tint_t pmem_read(addr_t addr, addr_t* ret, bool bfirst) {
    if (ret) *ret = 0;
    return 100; // 100 cycles main memory latency
}

tint_t pmem_write(addr_t addr, word_t data, unsigned char mask, bool bfirst) {
    return 100;
}

size_t parse_size(const std::string& s) {
    size_t mult = 1;
    std::string num = s;
    if (s.ends_with("kB") || s.ends_with("KB")) { mult = 1024; num = s.substr(0, s.size()-2); }
    else if (s.ends_with("MB")) { mult = 1024*1024; num = s.substr(0, s.size()-2); }
    else if (s.ends_with("B")) { num = s.substr(0, s.size()-1); }
    return std::stoul(num) * mult;
}

int main(int argc, char** argv) {
    std::string trace_file;
    size_t l1i_size = 32 * 1024;
    size_t l1i_blksize = 64;
    size_t l1i_assoc = 8;
    size_t max_insts = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.find("--l1i-size=") == 0) {
            l1i_size = parse_size(arg.substr(11));
        } else if (arg.find("--l1i-blksize=") == 0) {
            l1i_blksize = parse_size(arg.substr(14));
        } else if (arg.find("--l1i-assoc=") == 0) {
            l1i_assoc = parse_size(arg.substr(12));
        } else if (arg.find("--max-insts=") == 0) {
            max_insts = std::stoul(arg.substr(12));
        } else if (arg.find("--debug-flags=") == 0) {
            debug::set_flags(arg.substr(14));
        } else {
            trace_file = arg;
        }
    }

    if (trace_file.empty()) {
        std::cerr << "Usage: " << argv[0] << " <trace_file> [options]\n";
        return 1;
    }

    TraceReader reader(trace_file.c_str());
    Pipeline pipeline;
    BimodalPredictor bpu(12); // Default 4K entries (2^12)
    CacheSimulator icache(l1i_size, l1i_blksize, l1i_assoc);

    TraceInst inst;
    word_t dummy_word;
    
    // Default data latency
    uint32_t data_latency = 100;

    while (reader.next(inst)) {
        if (max_insts > 0 && pipeline.stats.insts >= max_insts) break;

        // BPU Predict
        bool pred_taken = false;
        if (inst.is_branch) {
             pred_taken = bpu.predict(inst.pc);
        }

        bool real_taken = (inst.br_taken != 0);
        bool mispred = false;
        if (inst.is_branch) {
            mispred = (pred_taken != real_taken);
            bpu.update(inst.pc, real_taken);
        }

        g_tick = pipeline.get_total_cycles();
        tint_t fetch_lat = icache.read_req(inst.pc, &dummy_word);

        pipeline.process(inst, fetch_lat, data_latency, mispred);
    }

    // Stats
    std::cout << "Trace: " << trace_file << "\n";
    pipeline.stats.dump_stats();
    icache.stats.dump_stats();
    bpu.stats.dump_stats();
    
    json root;
    root["Pipeline"] = pipeline.stats.gen_json();
    root["iCache"] = icache.stats.gen_json();
    root["BPU"] = bpu.stats.gen_json();
    // std::cout << root.dump(4) << "\n";

    return 0;
}
