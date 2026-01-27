#include "BranchPredictor.hh"
#include "types.hh"
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
