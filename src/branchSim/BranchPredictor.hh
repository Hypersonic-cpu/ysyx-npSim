#pragma once
#include "../types.hh"
#include "base.hh"
#include "debug.hh"
#include "stats.hh"
#include <cstddef>
#include <cstdint>
#include <format>
#include <print>
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

  explicit BTBBase(const std::string& name, size_t entries_pow2)
      : SimObject(name)
      , table_(1 << entries_pow2) {}
  virtual addr_t lookup(addr_t pc) const = 0;
  virtual void update(addr_t pc, addr_t target) = 0;
  json
  stats_json() const override {
    return json{};
  }
  json
  config_json() const override {
    return json{{"entries", table_.size()}};
  }
  void
  reset_stats() override {}
  void
  dump_stats(std::ostream& os = std::cout) const override {}

protected:
  std::vector<BTBEntry> table_;
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
                         size_t tag_bits = 10, size_t target_bits = 20)
      : BTBBase(name, entries_pow2)
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
      std::cerr << std::format(ANSI_BG_RED "BPU Inaccuarte stats" ANSI_ALL_NONE)
                << std::endl;
    }
    return j;
  }

  void
  dump_stats(std::ostream& os = std::cout) const override {
    os << "BranchPredictor Stats:\n";
    os << "  Accesses: " << accesses << "\n";
    os << "  Misses: " << misses << "\n";
    os << "  Miss Rate: " << miss_rate() << "\n";
    os << "  Miss:: No Target: " << no_target << "\n";
    os << "  Miss:: Bad Pred: " << bad_pred << "\n";
    os << "  Miss:: Bad Target: " << bad_target << "\n";
  }

  void
  reset_stats() override {
    size_t accesses = 0;
    size_t notify = 0; // Should == accesses
    // Result checking
    size_t misses = 0;
    size_t no_target = 0;
    size_t bad_target = 0;
    size_t bad_pred = 0;
  }
};

class BranchPredictor : public SimObject {
public:
  BPStatsBase stats;

public:
  explicit BranchPredictor(const std::string& name)
      : SimObject(name)
      , stats(name) {}
  virtual ~BranchPredictor() = default;
  virtual bool predict(addr_t pc, addr_t target) = 0;
  virtual void update(addr_t pc, bool taken) = 0;
  virtual bool judge(bool taken_gold, bool taken_pred, addr_t tar_gold,
                     addr_t tar_pred);
  void
  reset_stats() override {
    stats.reset_stats();
  }
  void
  dump_stats(std::ostream& os = std::cout) const override {
    stats.dump_stats(os);
  }
  json
  config_json() const override {
    return json({});
  }
  json
  stats_json() const override {
    return stats.gen_json();
  }
}; // namespace branchSim

// Simple 2-bit bimodal predictor
class BimodalPredictor : public BranchPredictor {
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
    return j;
  }

private:
  const uint8_t init_state_;
  size_t mask_;
  std::vector<uint8_t> table_; // 2-bit saturating counters
  size_t index(addr_t pc) const;
};

// Always Taken Predictor
class AlwaysTakenPredictor : public BranchPredictor {
public:
  explicit AlwaysTakenPredictor()
      : BranchPredictor("AlwaysTaken") {}

  bool
  predict(addr_t pc, addr_t) override {
    stats.accesses++;
    return true;
  }

  void
  update(addr_t pc, bool taken) override {}
};

// Backward Taken, Forward Not Taken
class BTFNTPredictor : public BranchPredictor {
public:
  BTFNTPredictor()
      : BranchPredictor("BTFNTPredictor") {}

  bool
  predict(addr_t pc, addr_t target) override {
    stats.accesses++;
    return target < pc;
  }
  void
  update(addr_t pc, bool taken) override {}
};

// Return Address Stack Wrapper
/*
class RASPredictorWrapper : public BranchPredictor {
  std::shared_ptr<BranchPredictor> base_;
  std::vector<addr_t> stack_;
  size_t top_ = 0;
  size_t cap_;

public:
  RASPredictorWrapper(std::shared_ptr<BranchPredictor> base, size_t
entries) : BranchPredictor(base->name() + "+RAS") , base_(base) ,
stack_(entries) , cap_(entries) {}

  bool
  predict(addr_t pc, addr_t target, bool is_call, bool is_ret) override {
    if (is_ret) {
      return true;
    }
    return base_->predict(pc, target, is_call, is_ret);
  }

  void
  update(addr_t pc, bool taken, addr_t target, bool is_call,
         bool is_ret) override {
    if (is_call) {
      stack_[top_] = pc + 4; // Push return addr
      top_ = (top_ + 1) % cap_;
    } else if (is_ret) {
      top_ = (top_ + cap_ - 1) % cap_; // Pop
    }
    base_->update(pc, taken, target, is_call, is_ret);
  }

  // Delegate stats
  json
  stats_json() const override {
    return base_->stats_json();
  }
  json
  config_json() const override {
    return base_->config_json();
  }
  void
  reset_stats() override {
    base_->reset_stats();
  }
  void
  dump_stats(std::ostream& os) const override {
    base_->dump_stats(os);
  }
};
*/

} // namespace branchSim
