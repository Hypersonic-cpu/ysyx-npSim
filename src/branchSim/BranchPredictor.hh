#pragma once
#include "../types.hh"
#include <cstddef>
#include <cstdint>
#include <vector>
#include "stats.hh"
#include "debug.hh"

namespace branchSim {

class BranchPredictor {
public:
  virtual ~BranchPredictor() = default;
  virtual bool predict(addr_t pc) = 0;
  virtual void update(addr_t pc, bool taken) = 0;
};

// Simple 2-bit bimodal predictor
class BimodalPredictor : public BranchPredictor {
public:
  struct BPStats : public StatsBase {
    BPStats() : StatsBase("BimodalPredictor") {}
    size_t accesses = 0;
    size_t misses = 0;

    json gen_json() const override {
        json j;
        j["accesses"] = accesses;
        j["misses"] = misses;
        j["miss_rate"] = accesses > 0 ? (double)misses / accesses : 0.0;
        return j;
    }

    void dump_stats(std::ostream& os = std::cout) const override {
        os << "BranchPredictor Stats:\n";
        os << "  Accesses: " << accesses << "\n";
        os << "  Misses: " << misses << "\n";
        os << "  Miss Rate: " << (accesses > 0 ? (double)misses / accesses : 0.0) << "\n";
    }

    void reset_stats() override {
        accesses = 0;
        misses = 0;
    }
  } stats;

  explicit BimodalPredictor(size_t entries_pow2 = 12);
  bool predict(addr_t pc) override;
  void update(addr_t pc, bool taken) override;

private:
  size_t mask_;
  std::vector<uint8_t> table_; // 2-bit saturating counters
  size_t index(addr_t pc) const;
};

} // namespace branchSim
