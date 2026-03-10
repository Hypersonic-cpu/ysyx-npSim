#pragma once
#include "areaSim/AreaEst.hh"
#include "defines/base.hh"
#include "defines/debug.hh"
#include "defines/types.hh"
#include "stats.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace branchSim {

// ---- BTBBase ---------------------------------------------------------------

class BTBBase : public SimObject {
public:
  struct BTBEntry {
    addr_t pc_tag = 0;
    addr_t target = 0;
    bool valid = false;
    uint8_t type = 0; // 0=normal, 1=return
  };

  explicit BTBBase(const std::string& name, size_t entries_pow2,
                   bool sram_dff = true)
      : SimObject(name, nullptr)
      , table_(1 << entries_pow2)
      , sram_dff_(sram_dff) {}
  virtual ~BTBBase() = default;
  virtual addr_t lookup(addr_t pc) const = 0;
  virtual void update(addr_t pc, addr_t target) = 0;

  json stats_json() const override { return json{}; }
  json config_json() const override; // implemented in .cc
  void reset_stats() override {}
  void dump_stats(std::ostream& = std::cout) const override {}

  size_t num_entries() const { return table_.size(); }
  uint8_t entry_type(size_t idx) const {
    return idx < table_.size() ? table_[idx].type : 0;
  }
  void set_entry_type(size_t idx, uint8_t t) {
    if (idx < table_.size())
      table_[idx].type = t;
  }

protected:
  std::vector<BTBEntry> table_;
  bool sram_dff_;
};

// ---- CompressedBTB ---------------------------------------------------------

class CompressedBTB : public BTBBase {
public:
  // tag_shift_override: if > 0, overrides the default tag shift
  explicit CompressedBTB(const std::string& name, size_t entries_pow2,
                         size_t tag_bits = 10, size_t target_bits = 20,
                         bool sram_dff = true, size_t tag_shift_override = 0)
      : BTBBase(name, entries_pow2, sram_dff)
      , tag_mask_((1u << tag_bits) - 1u)
      , index_mask_((1u << entries_pow2) - 1u)
      , tag_shift_(tag_shift_override > 0 ? tag_shift_override
                                          : 2 + entries_pow2) {
    (void)target_bits; // retained for API compatibility, not stored
  }

  addr_t lookup(addr_t pc) const override;
  void update(addr_t pc, addr_t target) override;

private:
  addr_t tag_mask_;
  addr_t index_mask_;
  size_t tag_shift_;

  size_t index(addr_t pc) const { return (pc >> 2) & index_mask_; }
};

// ---- BPStatsBase -----------------------------------------------------------

struct BPStatsBase : public StatsBase {
  explicit BPStatsBase(const std::string& name)
      : StatsBase(name) {}

  size_t accesses = 0;   // total BPU predict() calls
  size_t misses = 0;     // total pipeline mispredictions
  size_t no_target = 0;  // taken but BTB miss
  size_t bad_target = 0; // taken but wrong BTB target
  size_t bad_pred = 0;   // wrong direction prediction
  size_t nonbr_mispred = 0; // BTB aliasing on non-branch insts (--bpu-no-predecode)
  size_t br_accesses = 0;   // branch instruction accesses

  double miss_rate() const {
    return br_accesses > 0 ? (double)misses / br_accesses : 0.0;
  }

  json gen_json() const override;
  void dump_stats(std::ostream& os = std::cout) const override;
  void reset_stats() override {
    accesses = 0;
    misses = 0;
    no_target = 0;
    bad_target = 0;
    bad_pred = 0;
    nonbr_mispred = 0;
    br_accesses = 0;
  }
};

// ---- BranchPred (direction predictor base) ---------------------------------

// State captured at prediction time and carried to EX for accurate update.
struct BPUPredResult {
  bool taken;
  uint8_t bht_cnt;   // 2-bit saturating counter at prediction time
  uint32_t ghr_snap; // GHR snapshot before prediction shift
};

class BranchPred : public SimObject {
public:
  BPStatsBase stats;

  explicit BranchPred(const std::string& name)
      : SimObject(name, &this->stats)
      , stats(name) {}
  virtual ~BranchPred() = default;
  // Predict and return the full prediction state for later update.
  // For stateful predictors (bimodal+GHR, gshare) the GHR is shifted
  // with the PREDICTED direction immediately inside predict().
  virtual BPUPredResult predict(addr_t pc, addr_t btb_target) = 0;
  // Update BHT using old_cnt and old_ghr captured at prediction time.
  // btb_hit: whether BTB hit was observed at prediction (used for
  // first-time initialization: taken && !btb_hit -> init counter to 2).
  virtual void update(addr_t pc, bool taken, bool btb_hit, uint8_t old_cnt,
                      uint32_t old_ghr) = 0;
  // Called on branch misprediction to restore speculative GHR state.
  // Default no-op for predictors without GHR.
  virtual void on_mispred(bool actual_taken, uint8_t old_cnt,
                          uint32_t old_ghr) {
    (void)actual_taken;
    (void)old_cnt;
    (void)old_ghr;
  }
  json config_json() const override { return json{{"area", area::area_json(0.0)}}; }
};

// ---- Simple stateless predictors (trivially inline) -----------------------

class AlwaysTakenPredictor : public BranchPred {
public:
  AlwaysTakenPredictor()
      : BranchPred("AlwaysTaken") {}
  BPUPredResult predict(addr_t, addr_t) override {
    stats.accesses++;
    return {true, 3, 0};
  }
  void update(addr_t, bool, bool, uint8_t, uint32_t) override {}
};

class NoBPU : public BranchPred {
public:
  NoBPU()
      : BranchPred("NoBPU") {}
  BPUPredResult predict(addr_t, addr_t) override {
    stats.accesses++;
    return {false, 0, 0};
  }
  void update(addr_t, bool, bool, uint8_t, uint32_t) override {}
};

class BTFNTPredictor : public BranchPred {
public:
  BTFNTPredictor()
      : BranchPred("BTFNTPredictor") {}
  BPUPredResult predict(addr_t pc, addr_t target) override {
    stats.accesses++;
    return {target < pc, 0, 0}; // backward = taken
  }
  void update(addr_t, bool, bool, uint8_t, uint32_t) override {}
};

// ---- BimodalPredictor ------------------------------------------------------

class BimodalPredictor : public BranchPred {
public:
  // ghr_bits: number of GHR bits to XOR with PC for BHT index.
  // When 0, plain PC-indexed bimodal (no GHR).
  explicit BimodalPredictor(const std::string& name, size_t entries_pow2,
                             uint8_t init_val = 1, size_t ghr_bits = 0);
  BPUPredResult predict(addr_t pc, addr_t btb_target) override;
  void update(addr_t pc, bool taken, bool btb_hit, uint8_t old_cnt,
              uint32_t old_ghr) override;
  void on_mispred(bool actual_taken, uint8_t old_cnt,
                  uint32_t old_ghr) override;
  json config_json() const override;

private:
  size_t mask_;
  size_t ghr_bits_;
  uint32_t ghr_mask_;
  uint32_t ghr_;                 // global history register
  std::vector<uint8_t> table_;  // 2-bit saturating counters

  size_t index(addr_t pc, uint32_t ghr) const {
    return ((pc >> 2) ^ ghr) & mask_;
  }
};

// ---- GSharePredictor -------------------------------------------------------

class GSharePredictor : public BranchPred {
public:
  explicit GSharePredictor(const std::string& name, size_t entries_pow2,
                           size_t history_len = 10, uint8_t init_val = 1);
  BPUPredResult predict(addr_t pc, addr_t btb_target) override;
  void update(addr_t pc, bool taken, bool btb_hit, uint8_t old_cnt,
              uint32_t old_ghr) override;
  void on_mispred(bool actual_taken, uint8_t old_cnt,
                  uint32_t old_ghr) override;
  json config_json() const override;

private:
  const size_t history_len_;
  size_t mask_;
  uint32_t hist_mask_;
  uint32_t global_history_;
  std::vector<uint8_t> table_;

  size_t index(addr_t pc, uint32_t ghr) const {
    return ((pc >> 2) ^ ghr) & mask_;
  }
};

// ---- TournamentPredictor ---------------------------------------------------

class TournamentPredictor : public BranchPred {
public:
  explicit TournamentPredictor(const std::string& name, size_t entries_pow2,
                               size_t history_len = 10);
  BPUPredResult predict(addr_t pc, addr_t btb_target) override;
  void update(addr_t pc, bool taken, bool btb_hit, uint8_t old_cnt,
              uint32_t old_ghr) override;
  void on_mispred(bool actual_taken, uint8_t old_cnt,
                  uint32_t old_ghr) override;
  json config_json() const override;

private:
  const size_t history_len_;
  size_t mask_;
  uint32_t hist_mask_;
  uint32_t global_history_;
  std::vector<uint8_t> local_table_;
  std::vector<uint8_t> global_table_;
  std::vector<uint8_t> selector_table_; // 0-1=local, 2-3=global

  size_t local_index(addr_t pc) const { return (pc >> 2) & mask_; }
  size_t global_index(addr_t pc, uint32_t ghr) const {
    return ((pc >> 2) ^ ghr) & mask_;
  }
};

// ---- NoBTB -----------------------------------------------------------------

class NoBTB : public BTBBase {
public:
  NoBTB()
      : BTBBase("NoBTB", 0) {}
  addr_t lookup(addr_t) const override { return 0; }
  void update(addr_t, addr_t) override {}
  json config_json() const override { return json{{"entries", 0}}; }
};

// ---- ReturnAddrStack -------------------------------------------------------

class ReturnAddrStack {
public:
  explicit ReturnAddrStack(size_t depth)
      : stack_(depth, 0)
      , tos_(0)
      , cnt_(0) {}

  void push(addr_t addr) {
    tos_ = (tos_ + 1) % stack_.size();
    stack_[tos_] = addr;
    if (cnt_ < stack_.size())
      cnt_++;
  }

  void pop() {
    if (cnt_ > 0) {
      tos_ = (tos_ == 0) ? stack_.size() - 1 : tos_ - 1;
      cnt_--;
    }
  }

  addr_t top() const { return cnt_ > 0 ? stack_[tos_] : 0; }
  bool valid() const { return cnt_ > 0; }
  size_t depth() const { return stack_.size(); }

private:
  std::vector<addr_t> stack_;
  size_t tos_;
  size_t cnt_;
};

// ---- BranchResult ----------------------------------------------------------

struct BranchResult {
  bool pred_taken;    // direction prediction
  addr_t pred_target; // BTB target (0 if miss)
  bool will_redirect; // pred_taken && btb_hit
  uint8_t bht_cnt;    // BHT counter at prediction time (for update)
  uint32_t ghr_snap;  // GHR snapshot before prediction shift (for update)
};

// ---- BranchUnit ------------------------------------------------------------
// Combines BPU (direction) + BTB (target) + RAS into a unified interface.

class BranchUnit : public SimObject {
public:
  BPStatsBase stats;

  explicit BranchUnit(std::unique_ptr<BranchPred> bpu,
                      std::unique_ptr<BTBBase> btb, size_t ras_depth = 0);

  // Disable predecode filter: predict for all instructions (not just branches)
  void set_no_predecode(bool v) { no_predecode_ = v; }

  // Called at IF. Skips BPU for non-branch instructions unless no_predecode_.
  BranchResult predict_at_fetch(addr_t pc, bool is_branch);

  // Unconditional BPU + BTB lookup.
  BranchResult predict(addr_t pc);

  // Update BPU, BTB, and RAS after branch resolution.
  // pred: prediction state carried from IF; mispred: was prediction wrong.
  void update(addr_t pc, bool taken, addr_t target, bool is_call = false,
              bool is_ret = false, bool btb_hit = true,
              const BranchResult& pred = {false, 0, false, 1, 0},
              bool mispred = false);

  // Check prediction accuracy and record miss statistics.
  bool judge(bool real_taken, addr_t real_target, const BranchResult& pred);

  json stats_json() const override;
  json config_json() const override;
  void reset_stats() override;
  void dump_stats(std::ostream& os = std::cout) const override;

  std::string bpu_name() const { return bpu_->name(); }

private:
  std::unique_ptr<BranchPred> bpu_;
  std::unique_ptr<BTBBase> btb_;
  std::unique_ptr<ReturnAddrStack> ras_;
  bool no_predecode_ = false;
};

} // namespace branchSim
