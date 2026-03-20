#include "cacheSim/ReplPolicy.hh"

#include <algorithm>
#include <cassert>

namespace cacheSim {

CacheLine*
LRUReplPolicy::getVictim(const Set& set) {
  assert(!set.empty());
  auto* non_const = const_cast<Set*>(&set);
  auto it = std::min_element(non_const->begin(), non_const->end(),
                             [](const CacheLine& a, const CacheLine& b) {
                               return a.stamp < b.stamp;
                             });
  return &(*it);
}

void
LRUReplPolicy::onHit(CacheLine& line) {
  line.stamp = curr_tick();
}

void
LRUReplPolicy::onFill(CacheLine& line) {
  line.stamp = curr_tick();
}

CacheLine*
SRRIPReplPolicy::getVictim(const Set& set) {
  assert(!set.empty());
  auto* non_const = const_cast<Set*>(&set);
  auto it = std::max_element(non_const->begin(), non_const->end(),
                             [](const CacheLine& a, const CacheLine& b) {
                               return a.stamp < b.stamp;
                             });
  return &(*it);
}

void
SRRIPReplPolicy::onHit(CacheLine& line) {
  line.stamp = 0;
}

void
SRRIPReplPolicy::onFill(CacheLine& line) {
  // SRRIP insert at long re-reference distance (2-bit RRPV max=3).
  line.stamp = 2;
}

CacheLine*
RoundRobinReplPolicy::getVictim(const Set& set) {
  assert(!set.empty());
  auto* non_const = const_cast<Set*>(&set);
  auto idx = ptr_ % assoc_;
  return &non_const->at(idx);
}

void
RoundRobinReplPolicy::onHit(CacheLine& line) {}

void
RoundRobinReplPolicy::onFill(CacheLine& line) {
  ptr_ = (ptr_ + 1) % assoc_;
}

std::unique_ptr<ReplPolicy>
make_repl_policy(const std::string& name, size_t assoc) {
  if (name == "lru")
    return std::make_unique<LRUReplPolicy>();
  if (name == "srrip")
    return std::make_unique<SRRIPReplPolicy>();
  if (name == "rr")
    return std::make_unique<RoundRobinReplPolicy>(assoc);
  assert(false && "Unknown replacement policy");
  return std::make_unique<LRUReplPolicy>();
}

} // namespace cacheSim
