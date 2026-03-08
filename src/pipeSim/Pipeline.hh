#pragma once
#include "areaSim/AreaEst.hh"
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

// 5-stage in-order pipeline.
//
// Branch misprediction model (matches RTL FetchStage):
//
// At IF we already know the branch outcome from the trace.
// If mispredicted, the IFU continues fetching wrong-path PCs
// (polluting iCache) until the branch reaches EX stage where
// the hardware flush occurs.  At that point the IFQ is flushed
// and the IFU redirects to the correct target.
//
// FetchStage keeps issuing ar requests at sequential PCs until
// ExecuteStage signals flushWire via fromEx.valid && brex.take,
// which invalidates all validBuf entries and redirects pc.
//
class Pipeline final : public Processor {
public:
  // Matches RTL CycBreakdown categories (exclusive, sum = cycles)
  enum StallCause {
    NoStall = 0,
    NoInst,    // IFU stall (iCache miss, no fetch ready)
    LsuStall,  // LSU blocked (load/store in flight)
    BrMispred, // Branch misprediction recovery
    RAW,       // Read-after-write hazard (including WAW from M-ext)
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
                    size_t stq_size, BranchUnit* bpu, tick_t br_mis_pen = 1,
                    tick_t mmio_lat = 1);

  json
  config_json() const override {
    json j;
    j["BranchPenaltyCycles"] = BranchMissPenalty;
    j["MmioLatency"] = mmio_lat_;
    j["IFQSize"] = ifq_size_;
    j["area"] = area::area_json(19570.0);
    return j;
  }

  tick_t
  next_update() const override {
    return calc_nxtupd_;
  }

  void
  reset_stats() override {
    SimObject::reset_stats();
    stall_.last_tick = curr_tick();
    stall_.reset_tick = curr_tick();
    stall_.cause = NoInst;
    stall_.in_br_recovery = false;
  }

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
    IntMulExt,
    IntDivExt,
    Memory,
    WriteBack,
    Num_PipeStage
  };

  static constexpr std::array<std::string, Num_PipeStage> StageName{
    "Fetch", "Decode", "Execute", "M-Mul", "M-Div", "Memory", "WrBack"};

  // A single instruction flowing through the pipeline
  struct Transaction {
    Inst trace_inst;
    BranchResult br_pred;
    bool br_mispred = false;
    bool is_wrong_path;
    bool wait_mem;

    explicit Transaction() = delete;
    explicit Transaction(const Inst& inst, bool wrong_path = false,
                         bool is_wait_mem = false) noexcept
        : trace_inst{inst}
        , br_pred{false, 0, false}
        , br_mispred{false}
        , is_wrong_path{wrong_path}
        , wait_mem{is_wait_mem} {}
  };
  using TransPtr = std::unique_ptr<Transaction>;

  // Cycles from EX flush until IFU can issue first correct-path
  // fetch.  In RTL this is 1 cycle (flushWire to next cycle fetch).
  tick_t BranchMissPenalty;

  using SimPipe = std::array<TransPtr, Num_PipeStage>;
  TransPtr input_buffer_;
  SimPipe sim_pipe_;

  using stage_t = void (Pipeline::*)();

  void do_fetch_0();
  void do_fetch_1();
  void do_decode();
  void do_execute();
  void do_mul_ext();
  void do_div_ext();
  void do_memory();
  void do_writeback();

  void handle_lsu_resp();
  void handle_ifu_resp();
  void send_lsu_req(addr_t addr, word_t data, uint8_t strb, bool is_write);
  void update_reg_time(uint8_t rd, tick_t when);
  void flush_false_btb_hit(const Transaction& trans);

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
    auto mins = InfTime;
    for (auto elem : stage_update_) {
      if (elem > curr_tick())
        mins = std::min(mins, elem);
    }
    mins = std::min(mins, mmio_resp_tick_);
    calc_nxtupd_ = mins;
  }

  std::array<tick_t, 32> reg_ready_;
  std::array<tick_t, Num_PipeStage> stage_update_;
  std::array<stage_t, Num_PipeStage> const stage_handler_;

private:
  size_t ongoing_insts_;
  tick_t calc_nxtupd_;

  // LSU stage selection (Div > Mul > Mem priority)
  PipeStage lsu_serving;

  // Stall attribution state
  struct StallAttr {
    tick_t last_tick{0};
    tick_t reset_tick{0};
    StallCause cause{NoInst};
    bool in_br_recovery{false};
    tick_t brmiss_attr_end{0};
  } stall_;

  bool
  prev_stage_valid(PipeStage curr) const noexcept {
    switch (curr) {
    case Fetch:
      return true;
    case Decode:
      return sim_pipe_.at(Fetch) != nullptr;
    case Execute:
      return sim_pipe_.at(Decode) != nullptr;
    case IntMulExt:
      return sim_pipe_.at(Decode) != nullptr;
    case IntDivExt:
      return sim_pipe_.at(Decode) != nullptr;
    case Memory:
      return (sim_pipe_.at(Execute) != nullptr
              || sim_pipe_.at(IntMulExt) != nullptr
              || sim_pipe_.at(IntDivExt) != nullptr);
    case WriteBack:
      return sim_pipe_.at(Memory) != nullptr;
    default:
      assert(false);
      return false;
    }
  }

  void
  flush_stall_cycles(tick_t until) {
    if (until <= stall_.last_tick)
      return;
    if (stall_.cause == BrMispred && until > stall_.brmiss_attr_end
        && stall_.brmiss_attr_end > stall_.last_tick) {
      auto gap1 = stall_.brmiss_attr_end - stall_.last_tick;
      stats.brmiss_stall += gap1;
      auto gap2 = until - stall_.brmiss_attr_end;
      stats.noinst += gap2;
      stall_.cause = NoInst;
      stall_.last_tick = until;
      return;
    }
    auto gap = until - stall_.last_tick;
    switch (stall_.cause) {
    case LsuStall:
      stats.lsu_stall += gap;
      break;
    case RAW:
      stats.raw_stall += gap;
      break;
    case BrMispred:
      stats.brmiss_stall += gap;
      break;
    default:
      stats.noinst += gap;
      break;
    }
    stall_.last_tick = until;
  }

  void
  set_stall(StallCause new_cause) {
    if (new_cause == stall_.cause)
      return;
    flush_stall_cycles(curr_tick());
    stall_.cause = new_cause;
  }

  // Fetch queue (models RTL FetchStage PipeDepth buffer)
  std::list<TransPtr> fetch_queue_;
  size_t ifq_size_;

  // Wrong-path fetch state. When a mispredicted branch enters IF,
  // subsequent fetches use wrong-path PCs until the branch reaches
  // EX.  SoC mode: IDU drains wrong-path IFQ entries at 1/cycle,
  // freeing slots (matches high-latency RTL contention).
  // NPC mode: entries stay until EX flushes (limits to IFQ_SIZE).
  struct FetchState {
    bool wrong_path{false};
    addr_t wrong_path_pc{0};  // next wrong-path PC to fetch
    tick_t resume_tick{0};    // first cycle IFU may fetch after flush
    size_t orphan_resps{0};   // in-flight iCache resps to discard
  } fetch_;

  // MMIO response timer (SoC non-cacheable accesses)
  tick_t mmio_resp_tick_{InfTime};
  tick_t mmio_lat_;

  // M-extension computation completion ticks
  tick_t mul_ready_tick_{0};
  tick_t div_ready_tick_{0};
};

} // namespace pipeSim
