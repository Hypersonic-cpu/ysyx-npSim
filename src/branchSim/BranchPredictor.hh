#pragma once
#include "../types.hh"
#include <cstddef>
#include <cstdint>
#include <vector>

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
  explicit BimodalPredictor(size_t entries_pow2 = 12);
  bool predict(addr_t pc) override;
  void update(addr_t pc, bool taken) override;

private:
  size_t mask_;
  std::vector<uint8_t> table_; // 2-bit saturating counters
  size_t index(uint64_t pc) const;
};

} // namespace branchSim
