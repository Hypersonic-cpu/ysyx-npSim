#include "branchSim/BranchPredictor.hh"
#include "defines/types.hh"
#include <cstdint>
#include <string>

using namespace branchSim;

addr_t
CompressedBTB::lookup(addr_t pc) const {
  auto idx = index(pc);
  const auto& ent = table_.at(idx);
  // Use full tag (no masking) to avoid aliasing in large BTBs
  auto pc_tag = pc >> (2 + index_bits_);
  auto hit = (ent.valid && ent.pc_tag == pc_tag);
  if (hit) {
    DPRINTF(BranchPred, "BTB Hit: PC=0x%x Idx=0x%x Tag=0x%x Target=0x%x", pc,
            idx, ent.pc_tag, table_[idx].target);
    return table_[idx].target;
  } else {
    DPRINTF(BranchPred, "BTB Miss: PC=0x%x Idx=0x%x Tag=0x%x", pc, idx,
            pc_tag);
    return 0;
  }
}

void
CompressedBTB::update(addr_t pc, addr_t target) {
  auto idx = index(pc);
  auto& ent = table_.at(idx);
  // Use full tag (no masking)
  ent.pc_tag = pc >> (2 + index_bits_);
  ent.target = target;
  ent.valid = true;
  DPRINTF(BranchPred, "BTB Update: PC=0x%x Idx=0x%x Tag=0x%x Target=0x%x",
          pc, idx, table_[idx].pc_tag, target);
}

BimodalPredictor::BimodalPredictor(const std::string& name,
                                   size_t table_pow2, uint8_t init_val)
    : BranchPredictor(name)
    , mask_((1 << table_pow2) - 1)
    , init_state_{init_val}
    , table_(1 << table_pow2, init_val) {}

bool
BimodalPredictor::predict(addr_t pc, addr_t) {
  stats.accesses++;
  bool pred = table_.at(index(pc)) >= 2;
  DPRINTF(BranchPred, "Predict: PC=0x%x Idx=0x%lx Val=%d Pred=%d", pc,
          index(pc), table_[index(pc)], pred);
  return pred;
}

void
BimodalPredictor::update(addr_t pc, bool taken) {
  size_t i = index(pc);
  DPRINTF(BranchPred, "Update: PC=0x%x Taken=%d OldVal=%d", pc, taken,
          table_[i]);

  auto& ent = table_.at(i);
  if (taken) {
    if (ent < 3)
      ent++;
  } else {
    if (ent > 0)
      ent--;
  }
}

size_t
BimodalPredictor::index(addr_t pc) const {
  return (pc >> 2) & mask_;
}

bool
BranchPredictor::judge(bool taken_gold, bool taken_pred, addr_t tar_gold,
                       addr_t tar_pred) {
  auto accurate = true;
  if (taken_gold ^ taken_pred) {
    accurate = false;
    stats.bad_pred++;
  } else if (taken_gold && taken_pred) {
    auto target_acc = tar_gold == tar_pred;
    auto target_empty = tar_pred == 0;
    // empty target Implies inaccuracy
    assert(!target_empty || !target_acc);
    stats.no_target += !target_acc && target_empty;
    stats.bad_target += !target_acc && !target_empty;
    accurate = target_acc;
  } else {
    // both not taken
    accurate = true;
  }
  stats.notify++;
  if (!accurate) {
    stats.misses++;
    DPRINTF(BranchPred,
            "Notify: Mispredicted Gold=0x%x Pred=0x%x Taken Gold=%d "
            "Pred=%d",
            tar_gold, tar_pred, taken_gold, taken_pred);
  }
  return accurate;
}

// GShare Predictor Implementation
GSharePredictor::GSharePredictor(const std::string& name, size_t table_pow2,
                                 size_t history_len, uint8_t init_val)
    : BranchPredictor(name)
    , history_len_{history_len}
    , mask_((1 << table_pow2) - 1)
    , init_state_{init_val}
    , global_history_{0}
    , table_(1 << table_pow2, init_val) {}

bool
GSharePredictor::predict(addr_t pc, addr_t) {
  stats.accesses++;
  bool pred = table_.at(index(pc)) >= 2;
  DPRINTF(BranchPred, "GShare Predict: PC=0x%x GH=0x%x Idx=0x%lx Val=%d Pred=%d",
          pc, global_history_, index(pc), table_[index(pc)], pred);
  return pred;
}

void
GSharePredictor::update(addr_t pc, bool taken) {
  size_t i = index(pc);
  DPRINTF(BranchPred, "GShare Update: PC=0x%x Taken=%d GH=0x%x -> 0x%x OldVal=%d",
          pc, taken, global_history_, (global_history_ << 1) | taken, table_[i]);

  auto& ent = table_.at(i);
  if (taken) {
    if (ent < 3)
      ent++;
  } else {
    if (ent > 0)
      ent--;
  }

  // Update global history (shift left and add new taken bit)
  global_history_ = ((global_history_ << 1) | (taken ? 1 : 0)) & ((1 << history_len_) - 1);
}

size_t
GSharePredictor::index(addr_t pc) const {
  // XOR PC with global history
  return ((pc >> 2) ^ global_history_) & mask_;
}

// Tournament Predictor Implementation
TournamentPredictor::TournamentPredictor(const std::string& name,
                                         size_t table_pow2, size_t history_len)
    : BranchPredictor(name)
    , history_len_{history_len}
    , mask_((1 << table_pow2) - 1)
    , global_history_{0}
    , local_table_(1 << table_pow2, 1)
    , global_table_(1 << table_pow2, 1)
    , selector_table_(1 << table_pow2, 1) {} // Start with slight bias to local

bool
TournamentPredictor::predict(addr_t pc, addr_t) {
  stats.accesses++;

  size_t local_idx = local_index(pc);
  size_t global_idx = global_index(pc);
  size_t selector_idx = local_idx; // Use PC to index selector

  bool local_pred = local_table_[local_idx] >= 2;
  bool global_pred = global_table_[global_idx] >= 2;

  // Selector: 0-1 = use local, 2-3 = use global
  bool use_global = selector_table_[selector_idx] >= 2;
  bool pred = use_global ? global_pred : local_pred;

  DPRINTF(BranchPred,
          "Tournament Predict: PC=0x%x GH=0x%x Local=%d Global=%d Sel=%d UseGlobal=%d Pred=%d",
          pc, global_history_, local_pred, global_pred,
          selector_table_[selector_idx], use_global, pred);

  return pred;
}

void
TournamentPredictor::update(addr_t pc, bool taken) {
  size_t local_idx = local_index(pc);
  size_t global_idx = global_index(pc);
  size_t selector_idx = local_idx;

  bool local_pred = local_table_[local_idx] >= 2;
  bool global_pred = global_table_[global_idx] >= 2;

  bool local_correct = (local_pred == taken);
  bool global_correct = (global_pred == taken);

  // Update selector: increment if global better, decrement if local better
  if (local_correct != global_correct) {
    auto& selector = selector_table_[selector_idx];
    if (global_correct) {
      if (selector < 3)
        selector++;
    } else {
      if (selector > 0)
        selector--;
    }
  }

  // Update local predictor
  auto& local_ent = local_table_[local_idx];
  if (taken) {
    if (local_ent < 3)
      local_ent++;
  } else {
    if (local_ent > 0)
      local_ent--;
  }

  // Update global predictor
  auto& global_ent = global_table_[global_idx];
  if (taken) {
    if (global_ent < 3)
      global_ent++;
  } else {
    if (global_ent > 0)
      global_ent--;
  }

  DPRINTF(BranchPred,
          "Tournament Update: PC=0x%x Taken=%d GH=0x%x LocalCorr=%d GlobalCorr=%d",
          pc, taken, global_history_, local_correct, global_correct);

  // Update global history
  global_history_ = ((global_history_ << 1) | (taken ? 1 : 0)) & ((1 << history_len_) - 1);
}

size_t
TournamentPredictor::local_index(addr_t pc) const {
  return (pc >> 2) & mask_;
}

size_t
TournamentPredictor::global_index(addr_t pc) const {
  return ((pc >> 2) ^ global_history_) & mask_;
}
