#include "BranchPredictor.hh"
#include "types.hh"

using namespace branchSim;

addr_t
CompressedBTB::lookup(addr_t pc) const {
  auto idx = index(pc);
  const auto& ent = table_.at(idx);
  auto hit = (ent.valid && ent.pc_tag == masked_tag(pc));
  if (hit) {
    DPRINTF(BranchPred, "BTB Hit: PC=0x%x Idx=0x%x Tag=0x%x Target=0x%x", pc,
            idx, ent.pc_tag, table_[idx].target);
    return table_[idx].target;
  } else {
    DPRINTF(BranchPred, "BTB Miss: PC=0x%x Idx=0x%x Tag=0x%x", pc, idx,
            masked_tag(pc));
    return 0;
  }
}

void
CompressedBTB::update(addr_t pc, addr_t target) {
  auto idx = index(pc);
  auto& ent = table_.at(idx);
  ent.pc_tag = masked_tag(pc);
  ent.target = target;
  ent.valid = true;
  DPRINTF(BranchPred, "BTB Update: PC=0x%x Idx=0x%x Tag=0x%x Target=0x%x",
          pc, idx, table_[idx].pc_tag, target);
}

BimodalPredictor::BimodalPredictor(size_t table_pow2)
    : BranchPredictor("BimodalPredictor")
    , stats("BimodalBrPred")
    , mask_((1 << table_pow2) - 1)
    , table_(1 << table_pow2, 3) {} // Init to Strongly Taken

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
  bool pred = table_[i] >= 2;
  DPRINTF(BranchPred, "Update: PC=0x%x Taken=%d OldVal=%d", pc, taken,
          table_[i]);

  if (taken) {
    if (table_[i] < 3)
      table_[i]++;
  } else {
    if (table_[i] > 0)
      table_[i]--;
  }
}

size_t
BimodalPredictor::index(addr_t pc) const {
  return (pc >> 2) & mask_;
}

void
BimodalPredictor::notify(bool taken_gold, bool taken_pred, addr_t tar_gold,
                         addr_t tar_pred) {
  auto taken_acc = taken_gold == taken_pred;
  auto target_acc = tar_gold == tar_pred;
  auto target_empty = tar_pred == 0;
  stats.notify++;
  if (!target_acc) {
    stats.misses++;
    stats.no_target += target_empty;
    stats.bad_pred += !taken_acc;
    stats.bad_target += taken_acc && !target_empty;
  }
}
