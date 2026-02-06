#pragma once
#include "branchSim/BranchPredictor.hh"
#include "cacheSim/CacheBase.hh"
#include "defines/base.hh"
#include "defines/interface.hh"
#include "defines/types.hh"
#include "stats.hpp"
#include "trace.hh"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <queue>
#include <string>
#include <utility>

namespace pipeSim {

using Inst = trace::TraceInst;
using Cache = cacheSim::CacheBase;
using branchSim::BranchResult;
using branchSim::BranchUnit;

class Processor : public ClockedObject {
public:
  Processor(const std::string& name, StatsBase* pstats, BranchUnit* bpu)
      : ClockedObject(name, pstats)
      , imem{nullptr}
      , dmem{nullptr}
      , bpu{bpu}
      , is_draining_{false} {}
  virtual ~Processor() {}
  virtual void recv_mem_resp(CpuTrans trans) = 0;
  virtual void ack_mem_avail(AckTrans ack) = 0;
  virtual bool inst_avail() const = 0;
  virtual void feed_inst(const Inst& inst) = 0;
  virtual bool is_finished() const = 0;

  void
  set_draining() {
    is_draining_ = true;
  }

  void
  set_cache_ports(Cache* l1i, Cache* l1d) {
    imem = l1i;
    dmem = l1d;
  }

protected:
  Cache* imem;
  Cache* dmem;
  BranchUnit* bpu;
  bool is_draining_;
};

class Pipeline final : public Processor {
public:
  struct PipelineStats : public StatsBase {
    PipelineStats(const std::string& name)
        : StatsBase(name) {}
    size_t insts = 0;
    size_t cycles = 0;
    size_t stalls = 0;
    size_t frontend_stalls = 0;    // ICache miss stalls
    size_t backend_stalls = 0;     // RAW hazard stalls
    size_t branch_miss_cycles = 0; // Branch misprediction penalty
    size_t flush_count = 0;
    size_t mem_stalls = 0; // Memory access stalls
    size_t branches = 0;   // Total branch instructions

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
      j["mem_stalls"] = mem_stalls;
      j["branch_miss_cycles"] = branch_miss_cycles;
      j["flush_count"] = flush_count;
      j["branches"] = branches;
      // Bottleneck analysis
      if (cycles > 0) {
        j["frontend_stall_pct"] = 100.0 * frontend_stalls / cycles;
        j["backend_stall_pct"] = 100.0 * backend_stalls / cycles;
        j["mem_stall_pct"] = 100.0 * mem_stalls / cycles;
        j["branch_miss_pct"] = 100.0 * branch_miss_cycles / cycles;
      }
      return j;
    }

    void
    dump_stats(std::ostream& os = std::cout) const override {
      os << "Pipeline Stats:\n";
      os << "  Insts: " << insts << "\n";
      os << "  Cycles: " << cycles << "\n";
      os << "  IPC: " << get_ipc() << "\n";
      os << "  Stalls: " << stalls << "\n";
      os << "    Frontend: " << frontend_stalls;
      if (cycles > 0)
        os << " (" << (100.0 * frontend_stalls / cycles) << "%)";
      os << "\n";
      os << "    Backend (RAW): " << backend_stalls;
      if (cycles > 0)
        os << " (" << (100.0 * backend_stalls / cycles) << "%)";
      os << "\n";
      os << "    Memory: " << mem_stalls;
      if (cycles > 0)
        os << " (" << (100.0 * mem_stalls / cycles) << "%)";
      os << "\n";
      os << "  BrMissCyc: " << branch_miss_cycles;
      if (cycles > 0)
        os << " (" << (100.0 * branch_miss_cycles / cycles) << "%)";
      os << "\n";
      os << "  Branches: " << branches << ", Flushes: " << flush_count
         << "\n";
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
      mem_stalls = 0;
      branches = 0;
    }
  } stats;

public:
  Pipeline() = delete;
  explicit Pipeline(const std::string& name, size_t ifq_size,
                    size_t stq_size, BranchUnit* bpu);

  json
  config_json() const override {
    json j;
    j["BranchPenaltyCycles"] = BranchMissPenalty;
    j["BranchPenaltyFetches"] = PenaltyFetchCount;
    return j;
  }

  tick_t
  next_update() const override {
    return calc_nxtupd_;
  }

  // Simulate all events before next IF time.
  // Should be called after the inst is feed, which
  // will set the next available IF tick.
  void update_impl() override;

  bool
  inst_avail() const override {
    return input_buffer_ == nullptr && !is_draining_;
  }

  void
  feed_inst(const Inst& inst) override {
    assert(input_buffer_ == nullptr);
    ongoing_insts_++;
    auto trans = std::make_unique<Transaction>(inst);
    input_buffer_ = std::move(trans);
    DPRINTF(Pipeline, "FeedInst PC=0x%08x Remain %lu", inst.pc,
            ongoing_insts_);
    do_fetch_0();
  }

  bool
  is_finished() const override {
    return ongoing_insts_ == 0;
  }

  void recv_mem_resp(CpuTrans trans) override;
  void ack_mem_avail(AckTrans ack) override;

protected:
  enum PipeStage {
    Fetch = 0,
    Decode,
    Execute,
    Memory,
    WriteBack,
    Num_PipeStage
  };

  static constexpr std::array<std::string, Num_PipeStage> StageName{
    "Fetch", "Decode", "Execute", "Memory", "WrBack"};

  struct Transaction {
    Inst trace_inst;
    // Branch prediction made at IF stage
    BranchResult br_pred;
    // Set at IF when misprediction detected
    bool br_mispred = false;
    // True if this is a speculative fetch after misprediction
    bool is_penalty_fetch;
    bool wait_mem;

    explicit Transaction() = delete;
    explicit Transaction(const Inst& inst, bool is_penalty = false,
                         bool is_wait_mem = false) noexcept
        : trace_inst{inst}
        , br_pred{false, 0, false}
        , br_mispred{false}
        , wait_mem{is_wait_mem}
        , is_penalty_fetch{is_penalty} {}
  };
  using TransPtr = std::unique_ptr<Transaction>;

  static constexpr tick_t BranchMissPenalty{
    0}; // Branch misprediction penalty cycles
  static constexpr size_t PenaltyFetchCount{
    4}; // Number of penalty fetches to issue

  using SimPipe = std::array<TransPtr, Num_PipeStage>;
  TransPtr input_buffer_;

  // sim_pipe_.at(Stage) is the OUTPUT of stage
  SimPipe sim_pipe_;

  using stage_t = void (Pipeline::*)();

  void do_fetch_0(); // Issue request
  void do_fetch_1(); // To decode
  void do_decode();
  void do_execute();
  void do_memory();
  void do_writeback();

  void handle_lsu_resp();
  void handle_ifu_resp();

  void send_lsu_req(addr_t addr, word_t data, uint8_t strb, bool is_write);
  void send_ifu_req(addr_t addr);

  void wakeup_pending();
  void update_reg_time(uint8_t rd, tick_t when);

  void
  schedule(PipeStage stage, tick_t when) {
    assert(when >= curr_tick());
    stage_update_.at(stage) = when;
  }

  void
  async_schedule(PipeStage stage, tick_t when) {
    schedule(stage, when);
    calc_nxtupd_ = std::min(calc_nxtupd_, when);
  }

  void
  calc_sched() {

    if (debug::enabled_flags & debug::Event) [[unlikely]] {
      std::cerr << "[Event]  Calc StageUpdate {";
      for (const auto& st : stage_update_) {
        std::cerr << std::dec << st << ", ";
      }
      std::cerr << "} T@ " << curr_tick() << "\n";
    }
    auto mins = InfTime;
    for (auto elem : stage_update_) {
      if (elem > curr_tick()) {
        mins = std::min(mins, elem);
      }
    }
    calc_nxtupd_ = mins;
  }

  std::array<tick_t, 32> reg_ready_;

  // When OUTPUT of current stage is valid
  std::array<tick_t, Num_PipeStage> stage_update_;
  std::array<stage_t, Num_PipeStage> const stage_handler_;

private:
  size_t ongoing_insts_;
  tick_t calc_nxtupd_;

  // Queue of penalty fetch PCs to issue after misprediction
  std::queue<addr_t> penalty_inst_queue_;
  std::list<TransPtr> fetch_inst_queue_;
  size_t ifq_size_;
};

} // namespace pipeSim
