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
#include <deque>
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
    size_t wp_bpu_queries = 0;
    size_t wp_btb_hits = 0;
    size_t wp_redirects = 0;
    size_t er_bubble_accesses = 0;
    size_t wp_budget_caps = 0;

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
      j["WP_bpu_queries"] = wp_bpu_queries;
      j["WP_btb_hits"] = wp_btb_hits;
      j["WP_redirects"] = wp_redirects;
      j["ER_bubble_accesses"] = er_bubble_accesses;
      j["WP_budget_caps"] = wp_budget_caps;
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
                    tick_t mmio_lat = 1, size_t er_bubble = 3,
                    size_t wp_budget = 7);

  json
  config_json() const override {
    json j;
    j["BranchPenaltyCycles"] = BranchMissPenalty;
    j["MmioLatency"] = mmio_lat_;
    j["IFQSize"] = fetch_queue_.capacity();
    j["EarlyRedirectBubble"] = er_bubble_;
    j["WpBudgetPerMispred"] = wp_budget_;
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
        , br_pred{false, 0, false, 1, 0}
        , br_mispred{false}
        , is_wrong_path{wrong_path}
        , wait_mem{is_wait_mem} {}
  };
  using TransPtr = std::unique_ptr<Transaction>;

  // Fixed-capacity ring buffer for the instruction fetch queue.
  struct IFQRingBuf {
    std::vector<TransPtr> buf;
    size_t cap_;
    size_t head_ = 0, tail_ = 0, cnt_ = 0;

    explicit IFQRingBuf(size_t cap)
        : buf(cap)
        , cap_(cap) {}

    bool empty() const noexcept { return cnt_ == 0; }
    bool full() const noexcept { return cnt_ >= cap_; }
    size_t size() const noexcept { return cnt_; }
    size_t capacity() const noexcept { return cap_; }

    void push_back(TransPtr p) {
      assert(!full());
      buf[tail_] = std::move(p);
      tail_ = (tail_ + 1) % cap_;
      cnt_++;
    }

    TransPtr& front() noexcept {
      assert(!empty());
      return buf[head_];
    }

    void pop_front() noexcept {
      assert(!empty());
      buf[head_].reset();
      head_ = (head_ + 1) % cap_;
      cnt_--;
    }

    // Index access: at(0) = front, at(size-1) = back.
    TransPtr& at(size_t i) noexcept { return buf[(head_ + i) % cap_]; }

    void clear() noexcept {
      for (size_t i = 0; i < cnt_; i++)
        buf[(head_ + i) % cap_].reset();
      head_ = tail_ = cnt_ = 0;
    }

    // Find first entry satisfying predicate; returns nullptr if not found.
    template<class Pred>
    TransPtr* find_if_ptr(Pred&& pred) noexcept {
      for (size_t i = 0; i < cnt_; i++) {
        auto& e = buf[(head_ + i) % cap_];
        if (pred(e))
          return &e;
      }
      return nullptr;
    }
  };

  // Cycles from EX flush until IFU can issue first correct-path
  // fetch.  In RTL this is 1 cycle (flushWire to next cycle fetch).
  tick_t BranchMissPenalty;

  // earlyRedirect bubble size: RTL FetchStage sends sequential
  // iCache requests before BPU redirect fires (2-cycle iCache +
  // 1-cycle BPU SyncReadMem latency).
  size_t er_bubble_;

  // Max WP iCache accesses per misprediction. RTL FetchStage buffer
  // (PipeDepth+1=8) limits in-flight requests, capping WP.
  size_t wp_budget_;

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
    // Keep Pipeline active during wrong-path: do_fetch_0 issues
    // iCache requests each cycle but does not push to IFQ, so
    // there is no async_schedule callback to wake us.
    if (fetch_.wrong_path)
      mins = std::min(mins, curr_tick() + 1);
    // Wake at resume_tick so fetch resumes after branch penalty.
    if (fetch_.resume_tick > curr_tick() && input_buffer_)
      mins = std::min(mins, fetch_.resume_tick);
    calc_nxtupd_ = mins;
  }

  std::array<tick_t, 32> reg_ready_;
  std::array<tick_t, Num_PipeStage> stage_update_;
  std::array<stage_t, Num_PipeStage> const stage_handler_;

private:
  size_t ongoing_insts_;
  tick_t calc_nxtupd_;

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
      // EXU path only: MUL/DIV now bypass Memory and go directly to WB.
      return sim_pipe_.at(Execute) != nullptr;
    case WriteBack:
      return (sim_pipe_.at(Memory) != nullptr)
             || (sim_pipe_.at(IntMulExt) != nullptr
                 && curr_tick() >= mul_ready_tick_)
             || (sim_pipe_.at(IntDivExt) != nullptr
                 && curr_tick() >= div_ready_tick_);
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
  IFQRingBuf fetch_queue_;

  // Wrong-path fetch state. When a mispredicted branch enters IF,
  // wrong-path PCs are fetched as fire-and-forget iCache requests
  // (not pushed to IFQ).  Responses are tracked via resp_is_orphan_.
  struct FetchState {
    bool wrong_path{false};
    bool wp_flushed{false};   // true after EX flush; WP continues
    addr_t wrong_path_pc{0};  // next wrong-path PC to fetch
    tick_t resume_tick{0};    // first cycle IFU may fetch after flush
    size_t wp_count{0};       // WP fetches since misprediction start
    // iCache response ordering FIFO: tracks whether each in-flight
    // iCache request is for a real IFQ entry (false) or a
    // wrong-path/orphan request (true).  Responses arrive in order.
    std::deque<bool> resp_is_orphan;
    // Wrong-path BPU redirect (2-cycle delay matching RTL
    // iCache 1cyc + SyncReadMem 1cyc)
    bool wp_redirect_s1{false};
    addr_t wp_redirect_target_s1{0};
    bool wp_redirect_s2{false};
    addr_t wp_redirect_target_s2{0};
  } fetch_;

  // MMIO response timer (SoC non-cacheable accesses)
  tick_t mmio_resp_tick_{InfTime};
  tick_t mmio_lat_;

  // M-extension computation completion ticks
  tick_t mul_ready_tick_{0};
  tick_t div_ready_tick_{0};
};

} // namespace pipeSim
