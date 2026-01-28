#pragma once
#include "base.hh"
#include "branchSim/BranchPredictor.hh"
#include "cacheSim/CacheSimulator.hh"
#include "pipeSim/IOQueue.hh"
#include "stats.hh"
#include "trace.hh"
#include "types.hh"
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <queue>
#include <utility>
#include <vector>

extern void set_global_tick(tick_t t);

namespace pipeSim {

using Inst = trace::TraceInst;
using Cache = cacheSim::CacheSimulator;
using BrPred = branchSim::BranchPredictor;

class Pipeline final : public SimObject {

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

    double
    get_ipc() const {
      return cycles > 0 ? (double)insts / cycles : 0.0;
    }

    json
    gen_json() const override {
      json j;
      j["insts"] = insts;
      j["cycles"] = cycles;
      j["ipc"] = get_ipc();
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
      os << "  IPC: " << get_ipc() << "\n";
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

  explicit Pipeline(size_t ifq_size, size_t ldq_size, size_t stq_size,
                    Cache* iport, Cache* dport, BrPred* bpu)
      : SimObject("Pipeline")
      , reg_ready_{}
      , imem{iport}
      , dmem{dport}
      , bpu{bpu}
      , fetch_queue_(ifq_size)
      , memld_queue_(ldq_size)
      , memst_queue_(stq_size) {}

  // Simulate all events in next timestamp
  void iota_loop();

  bool
  is_finished() const {
    return sim_que_.empty();
  }

  bool
  fetch_avail() const {
    return !fetch_queue_.is_full();
  }

  bool
  feed_inst(const Inst& inst) {
    auto trans = std::make_unique<Transaction>(inst);
    do_fetch(std::move(trans));
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
  enum PipeStage {
    Fetch = 0,
    Decode,
    Execute,
    Memory,
    WriteBack,
    Num_PipeStage
  };

  struct Transaction {
    Inst trace_inst;
    tick_t finish_time;
    PipeStage next_stage;

    explicit Transaction() = delete;
    explicit Transaction(const Inst& inst)
        : trace_inst{inst}
        , finish_time{0}
        , next_stage{Fetch} {}
  };
  using TransPtr = std::unique_ptr<Transaction>;
  static constexpr tick_t BlockedTime{std::numeric_limits<tick_t>::max()};

  struct TransactionComparator {
    bool
    operator()(const TransPtr& lhs, const TransPtr& rhs) const {
      if (lhs->finish_time != rhs->finish_time)
        return lhs->finish_time > rhs->finish_time;
      return lhs->next_stage < rhs->next_stage;
    }
  };

  using SimQue = std::priority_queue<TransPtr, std::vector<TransPtr>,
                                     TransactionComparator>;
  SimQue sim_que_;

  using stage_t = void (*)(TransPtr);

  void do_fetch(TransPtr);
  // bool do_fetch(const Inst*);
  void do_decode(TransPtr);
  void do_execute(TransPtr);
  void do_memory(TransPtr);
  void do_writeback(TransPtr);

  void wakeup_pending();
  void update_raw_time(const TransPtr& updated);

  tick_t
  stage_avail_time(PipeStage target_stage) {
    auto future_stages =
      stage_ready_ | std::views::drop(static_cast<int>(target_stage));
    return std::ranges::max(future_stages);
  }

  // TransPtr pending_;
  std::list<TransPtr> pending_que_;
  std::pair<uint8_t, uint8_t> raw_rs_;

  // Cycle when register value is ready for consumption in EX stage
  std::array<tick_t, 32> reg_ready_;
  std::array<tick_t, Num_PipeStage> stage_ready_;
  std::array<stage_t, Num_PipeStage> stage_handler_;

  IOQueue fetch_queue_;
  // Currently unused. This RTL version has a 2-entry store
  // buffer but no dCache. So load will block the LSU when buffer miss.
  IOQueue memld_queue_;
  IOQueue memst_queue_;

  Cache* imem;
  Cache* dmem;

  BrPred* bpu;

private:
  word_t dummy;
};

} // namespace pipeSim
