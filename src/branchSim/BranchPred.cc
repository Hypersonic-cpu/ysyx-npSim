#include "branchSim/BranchPred.hh"
#include "defines/types.hh"
#include <cassert>
#include <cmath>
#include <ostream>
#include <string>

using namespace branchSim;

// ============================================================================
// BPStatsBase
// ============================================================================

json
BPStatsBase::gen_json() const {
  json j;
  j["accesses"] = accesses;
  j["misses"] = misses;
  j["miss_no_target"] = no_target;
  j["miss_bad_pred"] = bad_pred;
  j["miss_bad_target"] = bad_target;
  j["nonbr_mispred"] = nonbr_mispred;
  j["br_accesses"] = br_accesses;
  j["miss_rate"] = miss_rate();
  return j;
}

void
BPStatsBase::dump_stats(std::ostream& os) const {
  os << "BranchPred Stats:\n";
  os << "  Accesses: " << accesses << "\n";
  os << "  Misses: " << misses << "\n";
  os << "  Miss Rate: " << miss_rate() << "\n";
  os << "  Miss:: No Target: " << no_target << "\n";
  os << "  Miss:: Bad Pred: " << bad_pred << "\n";
  os << "  Miss:: Bad Target: " << bad_target << "\n";
}

// ============================================================================
// BTBBase
// ============================================================================

json
BTBBase::config_json() const {
  size_t n = table_.size();
  size_t idx_bits = n > 1 ? static_cast<size_t>(std::log2(n)) : 0;
  size_t tag_bits = 32 - idx_bits;
  if (sram_dff_) {
    size_t total_bits = n * (tag_bits + 32 + 1);
    return json{{"entries", n}, {"area", area::area_json(0.0, total_bits, 0.10)}};
  }
  json ar = area::area_json(0.0, n * 2, 0.10);
  ar["cacti_objs"] = json::array({area::sram_macro("btb_tag", n * tag_bits),
                                  area::sram_macro("btb_target", n * 32)});
  return json{{"entries", n}, {"area", ar}};
}

// ============================================================================
// CompressedBTB
// ============================================================================

addr_t
CompressedBTB::lookup(addr_t pc) const {
  auto idx = index(pc);
  const auto& ent = table_.at(idx);
  bool hit = ent.valid && (ent.pc_tag == ((pc >> tag_shift_) & tag_mask_));
  DPRINTF(BranchPred, "BTB %s: PC=0x%x Idx=0x%zx Target=0x%x",
          hit ? "Hit" : "Miss", pc, idx, hit ? table_[idx].target : 0u);
  return hit ? table_[idx].target : 0;
}

void
CompressedBTB::update(addr_t pc, addr_t target) {
  auto idx = index(pc);
  auto& ent = table_.at(idx);
  ent.pc_tag = (pc >> tag_shift_) & tag_mask_;
  ent.target = target;
  ent.valid = true;
  DPRINTF(BranchPred, "BTB Update: PC=0x%x Idx=0x%zx Target=0x%x", pc, idx, target);
}

void
CompressedBTB::clear_entry(addr_t pc) {
  auto idx = index(pc);
  auto& ent = table_.at(idx);
  ent.valid = false;
  ent.pc_tag = 0;
  ent.target = 0;
  ent.type = 0;
  DPRINTF(BranchPred, "BTB Clear: PC=0x%x Idx=0x%zx", pc, idx);
}

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
          "Tournament Predict: PC=0x%x GH=0x%x Local=%d Global=%d UseGlobal=%d Pred=%d",
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
    if (global_correct) { if (sel < 3) sel++; } else { if (sel > 0) sel--; }
  }

  auto& le = local_table_[li];
  if (taken) { if (le < 3) le++; } else { if (le > 0) le--; }

  auto& ge = global_table_[gi];
  if (taken) { if (ge < 3) ge++; } else { if (ge > 0) ge--; }
  // GHR is NOT shifted here; it was shifted at predict time.
}

void
TournamentPredictor::on_mispred(bool actual_taken, uint8_t, uint32_t old_ghr) {
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

// ============================================================================
// BranchUnit
// ============================================================================

BranchUnit::BranchUnit(std::unique_ptr<BranchPred> bpu,
                       std::unique_ptr<BTBBase> btb, size_t ras_depth)
    : SimObject("BranchUnit", &this->stats)
    , stats("BranchUnit")
    , bpu_(std::move(bpu))
    , btb_(std::move(btb)) {
  assert(bpu_ && "BPU must not be null");
  if (!btb_)
    btb_ = std::make_unique<NoBTB>();
  if (ras_depth > 0)
    ras_ = std::make_unique<ReturnAddrStack>(ras_depth);
}

BranchResult
BranchUnit::predict_at_fetch(addr_t pc, bool is_branch) {
  if (!no_predecode_ && !is_branch)
    return {false, 0, false, 0, 0};
  if (is_branch)
    stats.br_accesses++;
  return predict(pc);
}

BranchResult
BranchUnit::predict(addr_t pc) {
  stats.accesses++;
  // Model 1RW SRAM port conflict (RTL RegNext bypass).
  // bypass_valid_ is set from the PREVIOUS tick's write.
  // Same-index reads use bypass data (correct); different-index reads
  // are forced to BTB miss.
  addr_t btb_target = btb_->lookup(pc);
  if (bypass_valid_) {
    size_t read_idx = (pc >> 2) & (btb_->num_entries() - 1);
    if (read_idx != bypass_idx_) {
      // Different index from last write: SRAM output is stale
      btb_target = 0;
    }
    // Same index: SRAM output is correct (bypass data)
  }
  bool btb_hit = (btb_target != 0);
  auto [pred_taken, bht_cnt, ghr_snap] = bpu_->predict(pc, btb_target);
  addr_t target = btb_target;

  // RAS override for return instructions
  if (btb_hit && ras_) {
    auto idx = (pc >> 2) & (btb_->num_entries() - 1);
    if (btb_->entry_type(idx) == 1 && ras_->valid()) {
      target = ras_->top();
      pred_taken = true;
    }
  }

  bool will_redirect = pred_taken && (target != 0);
  DPRINTF(BranchPred,
          "BranchUnit Predict: PC=0x%08x taken=%d target=0x%08x redirect=%d"
          " bypass_conflict=%d",
          pc, pred_taken, target, will_redirect,
          bypass_valid_ && ((pc >> 2) & (btb_->num_entries() - 1)) != bypass_idx_);
  return {pred_taken, target, will_redirect, bht_cnt, ghr_snap};
}

void
BranchUnit::update(addr_t pc, bool taken, addr_t target, bool is_call,
                   bool is_ret, bool btb_hit, const BranchResult& pred,
                   bool mispred) {
  // BHT update gate: only update when BTB hit or taken.
  // Matches RTL: bhtWen = updValid && (updBtbHit || updTaken)
  if (btb_hit || taken)
    bpu_->update(pc, taken, btb_hit, pred.bht_cnt, pred.ghr_snap);
  if (mispred)
    bpu_->on_mispred(taken, pred.bht_cnt, pred.ghr_snap);
  if (taken) {
    btb_->update(pc, target);
    btb_written_this_tick_ = true;
    btb_written_idx_ = (pc >> 2) & (btb_->num_entries() - 1);
    auto idx = btb_written_idx_;
    btb_->set_entry_type(idx, is_ret ? 1 : 0);
  }
  if (ras_) {
    if (is_call) ras_->push(pc + 4);
    if (is_ret) ras_->pop();
  }
  DPRINTF(BranchPred, "BranchUnit Update: PC=0x%08x taken=%d target=0x%08x",
          pc, taken, target);
}

bool
BranchUnit::judge(bool real_taken, addr_t real_target,
                  const BranchResult& pred) {
  bool accurate;

  if (!real_taken && !pred.will_redirect) {
    accurate = true;
  } else if (real_taken && pred.will_redirect) {
    accurate = (pred.pred_target == real_target);
    if (!accurate) {
      stats.bad_target++;
      DPRINTF(BranchPred, "BranchUnit Mispred: bad_target real=0x%08x pred=0x%08x",
              real_target, pred.pred_target);
    }
  } else if (real_taken && !pred.will_redirect) {
    accurate = false;
    if (pred.pred_taken)
      stats.no_target++;
    else
      stats.bad_pred++;
    DPRINTF(BranchPred, "BranchUnit Mispred: %s",
            pred.pred_taken ? "no_target" : "bad_pred(NT)");
  } else {
    accurate = false;
    stats.bad_pred++;
    DPRINTF(BranchPred, "BranchUnit Mispred: bad_pred(T)");
  }

  if (!accurate)
    stats.misses++;
  return accurate;
}

json
BranchUnit::stats_json() const {
  return stats.gen_json();
}

json
BranchUnit::config_json() const {
  json j;
  j["bpu"] = bpu_->name();
  j["bpu_config"] = bpu_->config_json();
  j["btb"] = btb_->name();
  j["btb_config"] = btb_->config_json();

  size_t rd = ras_ ? ras_->depth() : 0;
  size_t total_bits =
    rd * 32 +
    (rd > 1 ? static_cast<size_t>(std::ceil(std::log2(rd))) : 0) +
    (rd > 0 ? static_cast<size_t>(std::ceil(std::log2(rd + 1))) : 0) +
    (rd > 0 ? btb_->num_entries() : 0) +
    (rd > 0 ? 2 : 0);
  j["ras_depth"] = rd;
  j["area"] = area::area_json(500.0, total_bits, 0.3);
  return j;
}

void
BranchUnit::reset_stats() {
  stats.reset_stats();
  bpu_->reset_stats();
  btb_->reset_stats();
}

void
BranchUnit::dump_stats(std::ostream& os) const {
  stats.dump_stats(os);
}
