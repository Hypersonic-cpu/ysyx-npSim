#pragma once
#include "base.hh"
#include "pipeSim/IOQueue.hh"
#include "stats.hh"
#include "trace.hh"

namespace pipeSim {

class Pipeline : public SimObject {
public:
  struct PipelineStats : public StatsBase {
    PipelineStats()
        : StatsBase("Pipeline") {}
    size_t insts = 0;
    size_t cycles = 0;
    size_t stalls = 0;
    size_t frontend_stalls = 0; // IFQ full
    size_t backend_stalls = 0;  // RAW
    size_t branch_miss_cycles = 0;
    size_t flush_count = 0;

    json
    gen_json() const override {
      json j;
      j["insts"] = insts;
      j["cycles"] = cycles;
      j["ipc"] = cycles > 0 ? (double)insts / cycles : 0.0;
      j["stalls"] = stalls;
      j["frontend_stalls"] = frontend_stalls;
      j["backend_stalls"] = backend_stalls;
      j["branch_miss_cycles"] = branch_miss_cycles;
      j["flush_count"] = flush_count;
      return j;
    }

    void
    dump_stats(std::ostream& os = std::cout) const override {
      os << "Pipeline Stats:\n";
      os << "  Insts: " << insts << "\n";
      os << "  Cycles: " << cycles << "\n";
      os << "  IPC: " << (cycles > 0 ? (double)insts / cycles : 0.0) << "\n";
      os << "  Stalls: " << stalls << "\n";
      os << "    Frontend: " << frontend_stalls << "\n";
      os << "    Backend: " << backend_stalls << "\n";
      os << "  BrMissCyc: " << branch_miss_cycles << "\n";
      os << "  Flushes: " << flush_count << "\n";
    }

    void
    reset_stats() override {
      insts = 0;
      cycles = 0;
      stalls = 0;
      frontend_stalls = 0;
      backend_stalls = 0;
      branch_miss_cycles = 0;
      flush_count = 0;
    }
  } stats;

  Pipeline() = delete;

  explicit Pipeline(size_t ifq_size, size_t ldq_size, size_t stq_size)
      : SimObject("Pipeline")
      , start_tick_(1)
      , lsu_tick_(4)
      , fetch_queue_(ifq_size)
      , memld_queue_(ldq_size)
      , memst_queue_(stq_size) {}

  // Simulate one instruction.
  // fetch_latency: cycles taken by iCache (including hit/miss latency).
  // data_latency: cycles taken by LSU (dCache or memory).
  // is_mispred: true if BPU mispredicted this instruction.
  void iota_inst(const trace::TraceInst& inst, tint_t fetch_lat,
                 tint_t load_lat, tint_t store_lat, bool is_mispred);

  tick_t
  icache_access_time() const {
    return std::max(fetch_queue_.next_avaiable(), start_tick_);
  }

  tick_t
  load_store_time() const {
    return std::max(lsu_tick_, std::max(memld_queue_.next_avaiable(),
                                        memst_queue_.next_avaiable()));
  }

  // SimObject Interface
  json
  stats_json() const override {
    return stats.gen_json();
  }

  json
  config_json() const override {
    json j;
    j["ifq_size"] = fetch_queue_.capacity();
    j["ldq_size"] = memld_queue_.capacity();
    j["stq_size"] = memst_queue_.capacity();
    return j;
  }

  void
  reset_stats() override {
    stats.reset_stats();
  }

  void
  dump_stats(std::ostream& os = std::cout) const override {
    stats.dump_stats(os);
  }

protected:
  // Cycle when register value is ready for consumption in EX stage
  tick_t reg_ready_[32] = {0};

  tick_t start_tick_;
  tick_t lsu_tick_;

  IOQueue fetch_queue_;
  IOQueue memld_queue_;
  IOQueue memst_queue_;
};

} // namespace pipeSim
