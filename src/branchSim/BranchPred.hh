#pragma once
#include "areaSim/AreaEst.hh"
#include "defines/types.hh"
#include "defines/base.hh"
#include "defines/debug.hh"
#include "stats.hpp"
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <vector>

namespace branchSim {

class BTBBase : public SimObject {
public:
  struct BTBEntry {
    addr_t pc_tag = 0;
    addr_t target = 0;
    bool valid = false;
    uint8_t type = 0;
  };

  explicit BTBBase(const std::string& name, size_t entries_pow2,
                   bool sram_dff = true)
      : SimObject(name, nullptr)
      , table_(1 << entries_pow2)
      , sram_dff_(sram_dff) {}
  virtual ~BTBBase() = default;
  virtual addr_t lookup(addr_t pc) const = 0;
  virtual void update(addr_t pc, addr_t target) = 0;
  json
  stats_json() const override {
    return json{};
  }
  json
  config_json() const override {
    size_t n = table_.size();
    size_t idx_bits = n > 1 ? static_cast<size_t>(std::log2(n)) : 0;
    size_t tag_bits = 32 - idx_bits;
    if (sram_dff_) {
      size_t entry_bits = tag_bits + 32 + 1;
      size_t total_bits = n * entry_bits;
      json ar = area::area_json(0.0, total_bits, 0.10);
      return json{{"entries", n}, {"area", ar}};
    }
    size_t valid_type_bits = n * 2;
    json ar = area::area_json(0.0, valid_type_bits, 0.10);
    ar["cacti_objs"] = json::array({
      area::sram_macro("btb_tag", n * tag_bits),
      area::sram_macro("btb_target", n * 32)
    });
    return json{{"entries", n}, {"area", ar}};
  }
  void
  reset_stats() override {}
  void
  dump_stats(std::ostream& os = std::cout) const override {}

  size_t num_entries() const { return table_.size(); }
  uint8_t entry_type(size_t idx) const {
    return idx < table_.size() ? table_[idx].type : 0;
  }
  void set_entry_type(size_t idx, uint8_t t) {
    if (idx < table_.size()) table_[idx].type = t;
  }

protected:
  std::vector<BTBEntry> table_;
  bool sram_dff_;
};

// Directly mapped
class CompressedBTB : public BTBBase {
private:
  auto
  index(addr_t pc) const {
    return (pc >> 2) & index_mask_;
  }
  auto
  masked_tag(addr_t pc) const {
    return (pc >> (2 + index_bits_)) & tag_mask_;
  }

public:
  explicit CompressedBTB(const std::string& name, size_t entries_pow2,
                         size_t tag_bits = 10, size_t target_bits = 20,
                         bool sram_dff = true)
      : BTBBase(name, entries_pow2, sram_dff)
      , tag_bits_(tag_bits)
      , tag_mask_((1 << tag_bits) - 1)
      , target_bits_(target_bits)
      , index_mask_((1 << entries_pow2) - 1)
      , index_bits_(entries_pow2) {}

  addr_t lookup(addr_t pc) const override;
  void update(addr_t pc, addr_t target) override;

private:
  size_t tag_bits_;
  addr_t tag_mask_;
  size_t index_bits_;
  addr_t index_mask_;
  size_t target_bits_;
};

struct BPStatsBase : public StatsBase {
  explicit BPStatsBase(const std::string& name)
      : StatsBase(name) {}

  // Request side
  size_t accesses = 0;
  size_t notify = 0; // Should == accesses
  // Result checking
  size_t misses = 0;
  size_t no_target = 0;
  size_t bad_target = 0;
  size_t bad_pred = 0;

  double
  miss_rate() const {
    return accesses > 0 ? (double)misses / accesses : 0.0;
  }

  json
  gen_json() const override {
    json j;
    j["accesses"] = accesses;
    j["notify_times"] = notify;
    j["misses"] = misses;
    j["miss_no_target"] = no_target;
    j["miss_bad_pred"] = bad_pred;
    j["miss_bad_target"] = bad_target;
    j["miss_rate"] = miss_rate();
    if (notify != accesses) {
      std::cerr << std::format(ANSI_BG_RED
                               "BPU Inaccuarte stats" ANSI_ALL_NONE)
                << std::endl;
    }
    return j;
  }

  void
  dump_stats(std::ostream& os = std::cout) const override {
    os << "BranchPred Stats:\n";
    os << "  Accesses: " << accesses << "\n";
    os << "  Misses: " << misses << "\n";
    os << "  Miss Rate: " << miss_rate() << "\n";
    os << "  Miss:: No Target: " << no_target << "\n";
    os << "  Miss:: Bad Pred: " << bad_pred << "\n";
    os << "  Miss:: Bad Target: " << bad_target << "\n";
  }

  void
  reset_stats() override {
    accesses = 0;
    notify = 0;
    misses = 0;
    no_target = 0;
    bad_target = 0;
    bad_pred = 0;
  }
};

class BranchPred : public SimObject {
public:
  BPStatsBase stats;

public:
  explicit BranchPred(const std::string& name)
      : SimObject(name, &this->stats)
      , stats(name) {}
  virtual ~BranchPred() = default;
  virtual bool predict(addr_t pc, addr_t target) = 0;
  virtual void update(addr_t pc, bool taken) = 0;
  virtual bool judge(bool taken_gold, bool taken_pred, addr_t tar_gold,
                     addr_t tar_pred);
  json
  config_json() const override {
    return json{{"area", area::area_json(0.0)}};
  }
};

// Simple 2-bit bimodal predictor
class BimodalPredictor : public BranchPred {
public:
  explicit BimodalPredictor(const std::string& name, size_t entries_pow2,
                            uint8_t init_val = 1);
  bool predict(addr_t pc, addr_t target) override;
  void update(addr_t pc, bool taken) override;

  // SimObject interface
  json
  config_json() const override {
    json j;
    j["entries"] = table_.size();
    json ar;
    ar["comb_percent"] = 0.30;
    ar["known_area"] = 0.0;
    ar["timing_bits"] = static_cast<int>(table_.size() * 2);
    j["area"] = ar;
    return j;
  }

private:
  const uint8_t init_state_;
  size_t mask_;
  std::vector<uint8_t> table_; // 2-bit saturating counters
  size_t index(addr_t pc) const;
};

// Always Taken Predictor
class AlwaysTakenPredictor : public BranchPred {
public:
  explicit AlwaysTakenPredictor()
      : BranchPred("AlwaysTaken") {}

  bool
  predict(addr_t pc, addr_t) override {
    stats.accesses++;
    return true;
  }

  void
  update(addr_t pc, bool taken) override {}
};

// Always Not-Taken Predictor (NoBPU - used when no BPU)
class NoBPU : public BranchPred {
public:
  explicit NoBPU()
      : BranchPred("NoBPU") {}

  bool
  predict(addr_t pc, addr_t) override {
    stats.accesses++;
    return false; // Always predict not-taken
  }

  void
  update(addr_t pc, bool taken) override {}
};

// Backward Taken, Forward Not Taken
class BTFNTPredictor : public BranchPred {
public:
  BTFNTPredictor()
      : BranchPred("BTFNTPredictor") {}

  bool
  predict(addr_t pc, addr_t target) override {
    stats.accesses++;
    return target < pc;
  }
  void
  update(addr_t pc, bool taken) override {}
};

// GShare Predictor: Uses global history XOR'd with PC
class GSharePredictor : public BranchPred {
public:
  explicit GSharePredictor(const std::string& name, size_t entries_pow2,
                           size_t history_len = 10, uint8_t init_val = 1);
  bool predict(addr_t pc, addr_t target) override;
  void update(addr_t pc, bool taken) override;

  json
  config_json() const override {
    json j;
    j["entries"] = table_.size();
    j["history_len"] = history_len_;
    size_t total_bytes = (table_.size() * 2 + 7) / 8;
    json ar;
    ar["comb_percent"] = 0.3;
    ar["known_area"] = 0.0;
    ar["timing_bits"] = history_len_;  // global history shift register = DFF
    ar["cacti_objs"] = json::array({
      area::sram_ram("bpu_table", total_bytes, 1)
    });
    j["area"] = ar;
    return j;
  }

private:
  const uint8_t init_state_;
  const size_t history_len_;
  size_t mask_;
  uint32_t global_history_;    // Shift register for global history
  std::vector<uint8_t> table_; // 2-bit saturating counters
  size_t index(addr_t pc) const;
};

// Tournament Predictor: Selector chooses between local (bimodal) and global
// (gshare)
class TournamentPredictor : public BranchPred {
public:
  explicit TournamentPredictor(const std::string& name, size_t entries_pow2,
                               size_t history_len = 10);
  bool predict(addr_t pc, addr_t target) override;
  void update(addr_t pc, bool taken) override;

  json
  config_json() const override {
    json j;
    j["entries"] = selector_table_.size();
    j["history_len"] = history_len_;
    // 3 tables of 2-bit counters + global history register
    size_t table_bits = selector_table_.size() * 2 * 3;
    size_t total_bytes = (table_bits + 7) / 8;
    json ar;
    ar["comb_percent"] = 0.3;
    ar["known_area"] = 0.0;
    ar["timing_bits"] = history_len_;  // global history shift register = DFF
    ar["cacti_objs"] = json::array({
      area::sram_ram("bpu_table", total_bytes, 1)
    });
    j["area"] = ar;
    return j;
  }

private:
  const size_t history_len_;
  size_t mask_;
  uint32_t global_history_;

  // Local predictor (bimodal)
  std::vector<uint8_t> local_table_;

  // Global predictor (gshare)
  std::vector<uint8_t> global_table_;

  // Selector: chooses between local (0) and global (1)
  // 2-bit counter: 00,01=use local, 10,11=use global
  std::vector<uint8_t> selector_table_;

  size_t local_index(addr_t pc) const;
  size_t global_index(addr_t pc) const;
};

// NoBTB - always returns 0 (miss)
class NoBTB : public BTBBase {
public:
  explicit NoBTB()
      : BTBBase("NoBTB", 0) {}

  json
  config_json() const override {
    return json{{"entries", 0}, {"area", area::area_json(0.0)}};
  }

  addr_t
  lookup(addr_t pc) const override {
    return 0; // Always miss
  }

  void
  update(addr_t pc, addr_t target) override {
    // No-op
  }
};

class ReturnAddrStack {
  std::vector<addr_t> stack_;
  size_t tos_ = 0;
  size_t cnt_ = 0;
  size_t depth_;

public:
  explicit ReturnAddrStack(size_t depth)
      : stack_(depth, 0), depth_(depth) {}

  void push(addr_t addr) {
    tos_ = (tos_ + 1) % depth_;
    stack_[tos_] = addr;
    if (cnt_ < depth_) cnt_++;
  }

  addr_t pop() {
    if (cnt_ == 0) return 0;
    addr_t val = stack_[tos_];
    tos_ = (tos_ == 0) ? depth_ - 1 : tos_ - 1;
    cnt_--;
    return val;
  }

  addr_t top() const {
    return cnt_ > 0 ? stack_[tos_] : 0;
  }

  bool valid() const { return cnt_ > 0; }
  size_t depth() const { return depth_; }
};

// Branch prediction result
struct BranchResult {
  bool pred_taken;    // BPU direction prediction
  addr_t pred_target; // BTB target (0 if miss)
  bool will_redirect; // pred_taken && btb_hit (actual redirect)
};

// BranchUnit: Combines BPU (direction) + BTB (target) + RAS into unified interface
class BranchUnit : public SimObject {
public:
  BPStatsBase stats;

private:
  std::unique_ptr<BranchPred> bpu_;
  std::unique_ptr<BTBBase> btb_;
  std::unique_ptr<ReturnAddrStack> ras_;

public:
  explicit BranchUnit(std::unique_ptr<BranchPred> bpu,
                      std::unique_ptr<BTBBase> btb,
                      size_t ras_depth = 0)
      : SimObject("BranchUnit", &this->stats)
      , stats("BranchUnit")
      , bpu_(std::move(bpu))
      , btb_(std::move(btb)) {
    assert(bpu_ && "BPU must not be null");
    if (!btb_) {
      btb_ = std::make_unique<NoBTB>();
    }
    if (ras_depth > 0) {
      ras_ = std::make_unique<ReturnAddrStack>(ras_depth);
    }
  }

  BranchResult
  predict(addr_t pc) {
    stats.accesses++;
    addr_t btb_target = btb_->lookup(pc);
    bool btb_hit = (btb_target != 0);
    bool pred_taken = bpu_->predict(pc, btb_target);

    // RAS override: if BTB marks entry as return type, use RAS
    addr_t target = btb_target;
    if (btb_hit && ras_) {
      auto idx = (pc >> 2) & (btb_->num_entries() - 1);
      if (btb_->entry_type(idx) == 1 && ras_->valid()) {
        target = ras_->top();
        pred_taken = true;
      }
    }

    bool will_redirect = pred_taken && (target != 0);
    DPRINTF(BranchPred,
            "BranchUnit Predict: PC=0x%08x pred_taken=%d target=0x%08x "
            "redirect=%d",
            pc, pred_taken, target, will_redirect);
    return {pred_taken, target, will_redirect};
  }

  void
  update(addr_t pc, bool taken, addr_t target,
         bool is_call = false, bool is_ret = false) {
    bpu_->update(pc, taken);
    if (taken) {
      btb_->update(pc, target);
      auto idx = (pc >> 2) & (btb_->num_entries() - 1);
      btb_->set_entry_type(idx, is_ret ? 1 : 0);
    }
    if (ras_) {
      if (is_call) ras_->push(pc + 4);
      if (is_ret) ras_->pop();
    }
    DPRINTF(BranchPred,
            "BranchUnit Update: PC=0x%08x taken=%d target=0x%08x", pc, taken,
            target);
  }

  bool
  judge(bool real_taken, addr_t real_target, const BranchResult& pred) {
    stats.notify++;
    bool accurate = true;

    if (!real_taken && !pred.will_redirect) {
      accurate = true;
    } else if (real_taken && pred.will_redirect) {
      if (pred.pred_target == real_target) {
        accurate = true;
      } else {
        accurate = false;
        stats.bad_target++;
        DPRINTF(BranchPred,
                "BranchUnit Mispred: bad_target real=0x%08x pred=0x%08x",
                real_target, pred.pred_target);
      }
    } else if (real_taken && !pred.will_redirect) {
      accurate = false;
      if (pred.pred_taken) {
        stats.no_target++;
        DPRINTF(BranchPred, "BranchUnit Mispred: no_target (BTB miss)");
      } else {
        stats.bad_pred++;
        DPRINTF(BranchPred, "BranchUnit Mispred: bad_pred (predicted NT)");
      }
    } else {
      accurate = false;
      stats.bad_pred++;
      DPRINTF(BranchPred, "BranchUnit Mispred: bad_pred (predicted T)");
    }

    if (!accurate) {
      stats.misses++;
    }
    return accurate;
  }

  json
  stats_json() const override {
    return stats.gen_json();
  }

  json
  config_json() const override {
    json j;
    j["bpu"] = bpu_->name();
    j["bpu_config"] = bpu_->config_json();
    j["btb"] = btb_->name();
    j["btb_config"] = btb_->config_json();
    size_t rd = ras_ ? ras_->depth() : 0;
    size_t ras_stack = rd * 32;
    size_t tos_bits  = rd > 1 ? static_cast<size_t>(std::ceil(std::log2(rd))) : 0;
    size_t cnt_bits  = rd > 0 ? static_cast<size_t>(std::ceil(std::log2(rd + 1))) : 0;
    size_t type_bits = rd > 0 ? btb_->num_entries() : 0;
    size_t byp_bits  = rd > 0 ? 2 : 0;
    size_t total = ras_stack + tos_bits + cnt_bits + type_bits + byp_bits;
    json ar = area::area_json(500.0, total, 0.3);
    j["ras_depth"] = rd;
    j["area"] = ar;
    return j;
  }

  void
  reset_stats() override {
    stats.reset_stats();
    bpu_->reset_stats();
    btb_->reset_stats();
  }

  void
  dump_stats(std::ostream& os = std::cout) const override {
    stats.dump_stats(os);
  }

  std::string
  bpu_name() const {
    return bpu_->name();
  }
};

} // namespace branchSim
