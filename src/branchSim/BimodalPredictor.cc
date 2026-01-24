#include "BranchPredictor.hh"

using namespace branchSim;

BimodalPredictor::BimodalPredictor(size_t entries_pow2)
    : mask_((1 << entries_pow2) - 1)
    , table_(1 << entries_pow2, 1) {} // Init to Weak Not Taken (1)

bool
BimodalPredictor::predict(addr_t pc) {
  stats.accesses++;
  bool pred = table_[index(pc)] >= 2;
  DPRINTF(BranchPred, "Predict: PC=0x%x Idx=0x%lx Val=%d Pred=%d", pc, index(pc), table_[index(pc)], pred);
  return pred;
}

void
BimodalPredictor::update(addr_t pc, bool taken) {
  size_t i = index(pc);
  bool pred = table_[i] >= 2;
  if (pred != taken) stats.misses++;
  DPRINTF(BranchPred, "Update: PC=0x%x Taken=%d OldVal=%d", pc, taken, table_[i]);

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
