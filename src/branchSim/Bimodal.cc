#include "branchSim/Bimodal.hh"
#include "defines/debug.hh"
#include <string>

using namespace branchSim;

// ============================================================================
// BimodalPredictor
// ============================================================================

BimodalPredictor::BimodalPredictor(const std::string& name, size_t table_pow2,
                                   uint8_t init_val)
    : BranchPred(name)
    , idx_bits_(table_pow2)
    , mask_((1u << table_pow2) - 1u)
    , table_(1u << table_pow2, init_val) {}

BPUPredResult
BimodalPredictor::predict(addr_t pc, addr_t) {
  stats.accesses++;
  size_t idx = index(pc);
  uint8_t cnt = table_.at(idx);
  bool pred = cnt >= 2;
  DPRINTF(BranchPred, "Bimodal Predict: PC=0x%x Idx=0x%zx Val=%d Pred=%d",
          pc, idx, cnt, pred);
  return {pred, cnt, 0};
}

void
BimodalPredictor::update(addr_t pc, bool taken, bool btb_hit, uint8_t old_cnt,
                         uint32_t) {
  size_t idx = index(pc);
  uint8_t new_cnt;
  if (taken && !btb_hit)
    new_cnt = 2; // first-time taken: initialize to weakly taken (matches RTL)
  else if (taken)
    new_cnt = (old_cnt < 3) ? old_cnt + 1 : 3;
  else
    new_cnt = (old_cnt > 0) ? old_cnt - 1 : 0;
  table_.at(idx) = new_cnt;
  DPRINTF(BranchPred, "Bimodal Update: PC=0x%x Idx=0x%zx %d->%d",
          pc, idx, old_cnt, new_cnt);
}

json
BimodalPredictor::config_json() const {
  json ar;
  ar["comb_percent"] = 0.30;
  ar["known_area"] = 0.0;
  ar["timing_bits"] = static_cast<int>(table_.size() * 2);
  return json{{"entries", table_.size()}, {"area", ar}};
}

// ============================================================================
// GSharePredictor
// ============================================================================

GSharePredictor::GSharePredictor(const std::string& name, size_t table_pow2,
                                 size_t history_len, uint8_t init_val)
    : BranchPred(name)
    , history_len_(history_len)
    , mask_((1u << table_pow2) - 1u)
    , hist_mask_((1u << history_len) - 1u)
    , global_history_(0)
    , table_(1u << table_pow2, init_val) {}

BPUPredResult
GSharePredictor::predict(addr_t pc, addr_t) {
  stats.accesses++;
  uint32_t snap = global_history_;
  size_t idx = index(pc, snap);
  uint8_t cnt = table_.at(idx);
  bool pred = cnt >= 2;
  DPRINTF(BranchPred,
          "GShare Predict: PC=0x%x GH=0x%x Idx=0x%zx Val=%d Pred=%d",
          pc, snap, idx, cnt, pred);
  // Shift GHR at predict time with PREDICTED direction.
  global_history_ = ((snap << 1) | (pred ? 1u : 0u)) & hist_mask_;
  return {pred, cnt, snap};
}

void
GSharePredictor::update(addr_t pc, bool taken, bool btb_hit, uint8_t old_cnt,
                        uint32_t old_ghr) {
  // Use GHR snapshot from prediction time to reconstruct the correct index.
  size_t idx = index(pc, old_ghr);
  uint8_t new_cnt;
  if (taken && !btb_hit)
    new_cnt = 2;
  else if (taken)
    new_cnt = (old_cnt < 3) ? old_cnt + 1 : 3;
  else
    new_cnt = (old_cnt > 0) ? old_cnt - 1 : 0;
  table_.at(idx) = new_cnt;
  // GHR is NOT shifted here; it was shifted at predict time.
}

void
GSharePredictor::on_mispred(bool actual_taken, uint8_t, uint32_t old_ghr) {
  // Restore GHR to reflect actual branch history.
  global_history_ = ((old_ghr << 1) | (actual_taken ? 1u : 0u)) & hist_mask_;
}

json
GSharePredictor::config_json() const {
  size_t total_bytes = (table_.size() * 2 + 7) / 8;
  json ar;
  ar["comb_percent"] = 0.3;
  ar["known_area"] = 0.0;
  ar["timing_bits"] = history_len_;
  ar["cacti_objs"] = json::array({area::sram_ram("bpu_table", total_bytes, 1)});
  return json{{"entries", table_.size()}, {"history_len", history_len_},
              {"area", ar}};
}

// ============================================================================
// TournamentPredictor
// ============================================================================

TournamentPredictor::TournamentPredictor(const std::string& name,
                                         size_t table_pow2, size_t history_len)
    : BranchPred(name)
    , history_len_(history_len)
    , mask_((1u << table_pow2) - 1u)
    , hist_mask_((1u << history_len) - 1u)
    , global_history_(0)
    , local_table_(1u << table_pow2, 1)
    , global_table_(1u << table_pow2, 1)
    , selector_table_(1u << table_pow2, 1) {}

BPUPredResult
TournamentPredictor::predict(addr_t pc, addr_t) {
  stats.accesses++;
  uint32_t snap = global_history_;
  size_t li = local_index(pc);
  size_t gi = global_index(pc, snap);
  bool local_pred = local_table_[li] >= 2;
  bool global_pred = global_table_[gi] >= 2;
  bool use_global = selector_table_[li] >= 2;
  bool pred = use_global ? global_pred : local_pred;
  DPRINTF(BranchPred,
          "Tournament: PC=0x%x GH=0x%x Local=%d Global=%d UseGlobal=%d Pred=%d",
          pc, snap, local_pred, global_pred, use_global, pred);
  // Shift GHR at predict time with PREDICTED direction.
  global_history_ = ((snap << 1) | (pred ? 1u : 0u)) & hist_mask_;
  return {pred, use_global ? global_table_[gi] : local_table_[li], snap};
}

void
TournamentPredictor::update(addr_t pc, bool taken, bool, uint8_t,
                            uint32_t old_ghr) {
  // Use GHR snapshot from prediction time for global index.
  size_t li = local_index(pc);
  size_t gi = global_index(pc, old_ghr);
  bool local_correct = (local_table_[li] >= 2) == taken;
  bool global_correct = (global_table_[gi] >= 2) == taken;

  if (local_correct != global_correct) {
    auto& sel = selector_table_[li];
    if (global_correct) {
      if (sel < 3) sel++;
    } else {
      if (sel > 0) sel--;
    }
  }

  auto& le = local_table_[li];
  if (taken) {
    if (le < 3) le++;
  } else {
    if (le > 0) le--;
  }

  auto& ge = global_table_[gi];
  if (taken) {
    if (ge < 3) ge++;
  } else {
    if (ge > 0) ge--;
  }
  // GHR is NOT shifted here; it was shifted at predict time.
}

void
TournamentPredictor::on_mispred(bool actual_taken, uint8_t,
                                uint32_t old_ghr) {
  global_history_ = ((old_ghr << 1) | (actual_taken ? 1u : 0u)) & hist_mask_;
}

json
TournamentPredictor::config_json() const {
  size_t total_bytes = (selector_table_.size() * 2 * 3 + 7) / 8;
  json ar;
  ar["comb_percent"] = 0.3;
  ar["known_area"] = 0.0;
  ar["timing_bits"] = history_len_;
  ar["cacti_objs"] = json::array({area::sram_ram("bpu_table", total_bytes, 1)});
  return json{{"entries", selector_table_.size()},
              {"history_len", history_len_},
              {"area", ar}};
}
