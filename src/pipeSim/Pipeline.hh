#pragma once
#include "branchSim/BranchPred.hh"
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
  // Matches RTL CycBreakdown categories (exclusive, sum = cycles)
  enum StallCause {
    NoStall = 0,
    NoInst,    // IFU stall (iCache miss, no fetch ready)
    LsuStall,  // LSU blocked (load/store in flight)
    BrMispred, // Branch misprediction recovery
    RAW,       // Read-after-write hazard
    NumCauses
  };

  struct PipelineStats : public StatsBase {
    PipelineStats(const std::string& name)
        : StatsBase(name) {}
    size_t insts = 0;
    size_t cycles = 0;
    size_t nostall = 0;
    size_t noinst = 0;
    size_t lsu_stall = 0;
    size_t brmiss_stall = 0;
    size_t raw_stall = 0;

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
      // Cycle breakdown (absolute and percentage)
      json bd;
      bd["NoStall"] = nostall;
      bd["NoInst"] = noinst;
      bd["LsuStall"] = lsu_stall;
      bd["BranchMispred"] = brmiss_stall;
      bd["RAW"] = raw_stall;
      if (cycles > 0) {
        bd["NoInst_pct"] = 100.0 * noinst / cycles;
        bd["LsuStall_pct"] = 100.0 * lsu_stall / cycles;
        bd["BranchMispred_pct"] = 100.0 * brmiss_stall / cycles;
        bd["RAW_pct"] = 100.0 * raw_stall / cycles;
      }
      j["CycBreakdown"] = bd;
      return j;
    }

    void
    dump_stats(std::ostream& os = std::cout) const override {
      os << "Pipeline Stats:\n";
      os << "  Insts: " << insts << "\n";
      os << "  Cycles: " << cycles << "\n";
      os << "  IPC: " << get_ipc() << "\n";
      os << "  BlockedCause:\n";
      os << "    NoStall: " << nostall << "\n";
      os << "    NoInst: " << noinst;
      if (cycles > 0)
        os << " (" << (100.0 * noinst / cycles) << "%)";
      os << "\n";
      os << "    LsuStall: " << lsu_stall;
      if (cycles > 0)
        os << " (" << (100.0 * lsu_stall / cycles) << "%)";
      os << "\n";
      os << "    BrMispred: " << brmiss_stall;
      if (cycles > 0)
        os << " (" << (100.0 * brmiss_stall / cycles) << "%)";
      os << "\n";
      os << "    RAW: " << raw_stall;
      if (cycles > 0)
        os << " (" << (100.0 * raw_stall / cycles) << "%)";
      os << "\n";
    }

    void
    reset_stats() override {
      insts = 0;
      cycles = 0;
      nostall = 0;
      noinst = 0;
      lsu_stall = 0;
      brmiss_stall = 0;
      raw_stall = 0;
    }
  } stats;

public:
  Pipeline() = delete;
  explicit Pipeline(const std::string& name, size_t ifq_size,
                    size_t stq_size, BranchUnit* bpu,
                    tick_t br_mis_pen = 10, size_t pf_count = 4);

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

  void
  reset_stats() override {
    SimObject::reset_stats();
    last_attr_tick_ = curr_tick();
    reset_tick_ = curr_tick();
    stall_cause_ = NoInst;
    in_penalty_recovery_ = false;
    deferred_br_penalty_ = 0;
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

  tick_t BranchMissPenalty;
  size_t PenaltyFetchCount;

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

  // Per-cycle stall tracking (matches RTL BlockedCause attribution)
  tick_t last_attr_tick_{0};
  StallCause stall_cause_{NoInst};
  tick_t deferred_br_penalty_{0};
  tick_t reset_tick_{0};

  // True while penalty fetches are in flight or penalty stall is active.
  // All IFU-idle cycles during this window are BrMispred, not NoInst.
  bool in_penalty_recovery_{false};

  void
  flush_stall_cycles(tick_t until) {
    if (until <= last_attr_tick_)
      return;
    auto gap = until - last_attr_tick_;
    switch (stall_cause_) {
    case LsuStall: stats.lsu_stall += gap; break;
    case RAW: stats.raw_stall += gap; break;
    default:
      if (in_penalty_recovery_) {
        stats.brmiss_stall += gap;
      } else {
        stats.noinst += gap;
      }
      break;
    }
    last_attr_tick_ = until;
  }

  // Transition stall cause: flush old cause's cycles, then set new.
  void
  set_stall(StallCause new_cause) {
    if (new_cause == stall_cause_)
      return;
    flush_stall_cycles(curr_tick());
    stall_cause_ = new_cause;
  }

  // Queue of penalty fetch PCs to issue after misprediction
  std::queue<addr_t> penalty_inst_queue_;
  tick_t penalty_stall_until_{0};
  tick_t last_mispred_tick_{0}; // For dynamic BrMisPen
  std::list<TransPtr> fetch_inst_queue_;
  size_t ifq_size_;
};

} // namespace pipeSim
