#pragma once
#include "branchSim/BranchPred.hh"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace branchSim {

// ---- BimodalPredictor ------------------------------------------------------

class BimodalPredictor : public BranchPred {
public:
  explicit BimodalPredictor(const std::string& name, size_t entries_pow2,
                             uint8_t init_val = 1);
  BPUPredResult predict(addr_t pc, addr_t btb_target) override;
  void update(addr_t pc, bool taken, bool btb_hit, uint8_t old_cnt,
              uint32_t old_ghr) override;
  json config_json() const override;

  // Expose index computation for use by TAGE base predictor.
  size_t index(addr_t pc) const {
    size_t lo = (pc >> 2) & mask_;
    size_t hi = (pc >> (2 + idx_bits_)) & mask_;
    return lo ^ hi;
  }

protected:
  size_t idx_bits_;
  size_t mask_;
  std::vector<uint8_t> table_; // 2-bit saturating counters
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

} // namespace branchSim
