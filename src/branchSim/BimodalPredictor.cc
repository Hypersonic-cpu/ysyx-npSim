#include "branchSim/BranchPredictor.hh"
#include <cmath>

using namespace branchSim;

BimodalPredictor::BimodalPredictor(size_t entries_pow2) {
  size_t entries = 1ull << entries_pow2;
  mask_ = entries - 1;
  table_.assign(entries, 2); // weakly taken
}

size_t
BimodalPredictor::index(addr_t pc) const {
  return (pc >> 2) & mask_;
}

bool
BimodalPredictor::predict(addr_t pc) {
  uint8_t c = table_.at(index(pc));
  return c >= 2;
}

void
BimodalPredictor::update(addr_t pc, bool taken) {
  auto& c = table_.at(index(pc));
  if (taken) {
    if (c < 3)
      ++c;
  } else {
    if (c > 0)
      --c;
  }
}
