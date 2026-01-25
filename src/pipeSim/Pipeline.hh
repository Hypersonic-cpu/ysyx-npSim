#pragma once
#include "trace.hh"
#include "pipeSim/IOQueue.hh"
#include "stats.hh"

namespace pipeSim {

class Pipeline {
public:
  struct PipelineStats : public StatsBase {
    PipelineStats()
        : StatsBase("Pipeline") {}
    size_t insts = 0;
    size_t cycles = 0;
    size_t stalls = 0;
    size_t raw_stalls = 0;
    size_t flush_count = 0;

    json
    gen_json() const override {
      json j;
      j["insts"] = insts;
      j["cycles"] = cycles;
      j["ipc"] = cycles > 0 ? (double)insts / cycles : 0.0;
      j["stalls"] = stalls;
      j["raw_stalls"] = raw_stalls;
      j["flush_count"] = flush_count;
      return j;
    }

    void
    dump_stats(std::ostream& os = std::cout) const override {
      os << "Pipeline Stats:\n";
      os << "  Insts: " << insts << "\n";
      os << "  Cycles: " << cycles << "\n";
      os << "  IPC: " << (cycles > 0 ? (double)insts / cycles : 0.0) << "\n";
      os << "  Stalls: " << stalls << " (RAW: " << raw_stalls << ")\n";
      os << "  Flushes: " << flush_count << "\n";
    }

    void
    reset_stats() override {
      insts = 0;
      cycles = 0;
      stalls = 0;
      raw_stalls = 0;
      flush_count = 0;
    }
  } stats;

  Pipeline() = delete;

  Pipeline(size_t ifq_size, size_t ldq_size, size_t stq_size)
      : start_tick_(1)
      , fetch_queue_(ifq_size)
      , memld_queue_(ldq_size)
      , memst_queue_(stq_size) {}

  // Simulate one instruction.
  // fetch_latency: cycles taken by iCache (including hit/miss latency).
  // data_latency: cycles taken by LSU (dCache or memory).
  // is_mispred: true if BPU mispredicted this instruction.
  void iota_inst(const trace::TraceInst& inst, tint_t fetch_latency,
                 tint_t load_latency, tint_t store_latency, bool is_mispred);

  size_t
  get_total_cycles() const {
    return stats.cycles;
  }

  size_t
  get_total_insts() const {
    return stats.insts;
  }

  size_t
  get_total_stalls() const {
    return stats.stalls;
  }

protected:
  // Cycle when register value is ready for consumption in EX stage
  tick_t reg_ready_[32] = {0};

  tick_t start_tick_;

  IOQueue fetch_queue_;
  IOQueue memld_queue_;
  IOQueue memst_queue_;
};

} // namespace pipeSim
