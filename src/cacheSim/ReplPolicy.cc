#include "cacheSim/ReplPolicy.hh"

#include <algorithm>
#include <cassert>

namespace cacheSim {

CacheLine*
LRUReplPolicy::getVictim(size_t set_idx, Set& set) {
  (void)set_idx;
  assert(!set.empty());
  auto it = std::min_element(set.begin(), set.end(),
                             [](const CacheLine& a, const CacheLine& b) {
                               return a.stamp < b.stamp;
                             });
  return &(*it);
}

void
LRUReplPolicy::onHit(size_t set_idx, size_t way_idx, CacheLine& line) {
  (void)set_idx;
  (void)way_idx;
  line.stamp = curr_tick();
}

void
LRUReplPolicy::onFill(size_t set_idx, size_t way_idx, CacheLine& line) {
  (void)set_idx;
  (void)way_idx;
  line.stamp = curr_tick();
}

CacheLine*
SRRIPReplPolicy::getVictim(size_t set_idx, Set& set) {
  (void)set_idx;
  assert(!set.empty());
  auto it = std::max_element(set.begin(), set.end(),
                             [](const CacheLine& a, const CacheLine& b) {
                               return a.stamp < b.stamp;
                             });
  return &(*it);
}

void
SRRIPReplPolicy::onHit(size_t set_idx, size_t way_idx, CacheLine& line) {
  (void)set_idx;
  (void)way_idx;
  line.stamp = 0;
}

void
SRRIPReplPolicy::onFill(size_t set_idx, size_t way_idx, CacheLine& line) {
  (void)set_idx;
  (void)way_idx;
  // SRRIP insert at long re-reference distance (2-bit RRPV max=3).
  line.stamp = 2;
}

CacheLine*
RoundRobinReplPolicy::getVictim(size_t set_idx, Set& set) {
  assert(!set.empty());
  auto idx = ptr_.at(set_idx) % assoc_;
  return &set.at(idx);
}

void
RoundRobinReplPolicy::onHit(size_t set_idx, size_t way_idx,
                            CacheLine& line) {
  (void)set_idx;
  (void)way_idx;
  (void)line;
}

void
RoundRobinReplPolicy::onFill(size_t set_idx, size_t way_idx,
                             CacheLine& line) {
  (void)way_idx;
  (void)line;
  ptr_.at(set_idx) = (ptr_.at(set_idx) + 1) % assoc_;
}

size_t
PLRUReplPolicy::victim_way(size_t set_idx) const {
  assert(assoc_ == 1 || assoc_ == 2 || assoc_ == 4);
  if (assoc_ == 1)
    return 0;
  const auto bits = bits_.at(set_idx);
  if (assoc_ == 2) {
    const bool b0 = (bits & 0x1) != 0;
    // RTL: Mux(plru(0), 0, 1)
    return b0 ? 0 : 1;
  }
  if (assoc_ == 4) {
    const bool b0 = (bits & 0x1) != 0;
    const bool b1 = (bits & 0x2) != 0;
    const bool b2 = (bits & 0x4) != 0;
    if (b0)
      return b1 ? 0 : 1;
    return b2 ? 2 : 3;
  }
  assert(false && "Unsupported assoc. Assoc in (1, 2, 4) only");
}

void
PLRUReplPolicy::update_bits(size_t set_idx, size_t way_idx) {
  assert(assoc_ == 1 || assoc_ == 2 || assoc_ == 4);
  if (assoc_ == 1) {
    bits_.at(set_idx) = 0;
    return;
  }
  if (assoc_ == 2) {
    // RTL: new b0 = way(0)
    bits_.at(set_idx) = static_cast<uint8_t>(way_idx & 0x1);
    return;
  }

  uint8_t b1 = (bits_.at(set_idx) >> 1) & 0x1;
  uint8_t b2 = (bits_.at(set_idx) >> 2) & 0x1;
  const uint8_t w0 = static_cast<uint8_t>(way_idx & 0x1);
  const uint8_t w1 = static_cast<uint8_t>((way_idx >> 1) & 0x1);
  const uint8_t newB0 = w1;
  const uint8_t newB1 = w1 ? b1 : w0;
  const uint8_t newB2 = w1 ? w0 : b2;
  bits_.at(set_idx) =
    static_cast<uint8_t>((newB0 << 0) | (newB1 << 1) | (newB2 << 2));
}

CacheLine*
PLRUReplPolicy::getVictim(size_t set_idx, Set& set) {
  assert(!set.empty());
  auto idx = victim_way(set_idx);
  return &set.at(idx);
}

void
PLRUReplPolicy::onHit(size_t set_idx, size_t way_idx, CacheLine& line) {
  (void)line;
  update_bits(set_idx, way_idx);
}

void
PLRUReplPolicy::onFill(size_t set_idx, size_t way_idx, CacheLine& line) {
  (void)line;
  update_bits(set_idx, way_idx);
}

std::unique_ptr<ReplPolicy>
make_repl_policy(const std::string& name, size_t assoc, size_t num_sets) {
  if (name == "plru")
    return std::make_unique<PLRUReplPolicy>(assoc, num_sets);
  if (name == "lru")
    return std::make_unique<LRUReplPolicy>();
  if (name == "srrip")
    return std::make_unique<SRRIPReplPolicy>();
  if (name == "rr")
    return std::make_unique<RoundRobinReplPolicy>(assoc, num_sets);
  assert(false && "Unknown replacement policy");
  return std::make_unique<PLRUReplPolicy>(assoc, num_sets);
}

} // namespace cacheSim
