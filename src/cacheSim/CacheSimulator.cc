
#include "cacheSim/CacheSimulator.hh"
#include "types.hh"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <string>
#include <unordered_map>

using namespace cacheSim;

CacheSimulator::CacheSimulator(size_t size_bytes, size_t line_bytes,
                               size_t assoc)
    : lineBytes_(line_bytes)
    , offsetBits_(floorLog2(line_bytes))
    , sets_(size_bytes / (line_bytes * assoc))
    , assoc_(assoc)
    , hitTime_(3)
    , setsArr_(sets_, std::vector<CacheLine>(assoc_, {line_bytes})) {
  assert(size_bytes % (line_bytes * assoc) == 0);
  assert(sets_ > 1 && isPowerOf2(sets_));
  assert(isPowerOf2(line_bytes));
}

addr_t
CacheSimulator::tagOf(addr_t addr) const {
  return blockAddrOf(addr);
}

size_t
CacheSimulator::setIndexOf(addr_t addr) const {
  return (addr >> offsetBits_) & (sets_ - 1);
}

size_t
CacheSimulator::offsetOf(addr_t addr) const {
  return addr & (lineBytes_ - 1);
}

addr_t
CacheSimulator::blockAddrOf(addr_t addr) const {
  return addr & ~(lineBytes_ - 1);
}

size_t
CacheSimulator::size() const {
  return sets_ * assoc_ * lineBytes_;
}
size_t
CacheSimulator::assoc() const {
  return assoc_;
}
size_t
CacheSimulator::blksize() const {
  return lineBytes_;
}
tint_t
CacheSimulator::latency() const {
  return hitTime_;
}

tint_t
CacheSimulator::read_req(addr_t addr, word_t* ret) {
  auto blk = access(addr);
  auto off = offsetOf(addr);
  assert(blk);
  if (blk->isValid()) {
    // hit
    *ret = blk->atAligned(off);
    return hitTime_;
  } else {
    // TODO: if dirty, write back;
    tint_t latency = hitTime_ + handle_fill(blk, addr);
    *ret = blk->atAligned(off);
    return latency;
  }
}

void
CacheSimulator::flush_all() {
  SIMPRINTFN(CACHE, "Flush All");
  for (auto& s : setsArr_) {
    for (auto& l : s) {
      l.invalidate();
    }
  }
}

tint_t
CacheSimulator::write_req(addr_t addr, word_t data, uint8_t mask) {
  assert(false && "Unimpl");
  // auto blk = access(addr);
  // auto off = offsetOf(addr);
  // assert(blk);
  // if (blk->isValid()) {
  //   // hit
  //   blk->atAligned(off) = val;
  //   return hitTime_;
  // } else {
}

tint_t
CacheSimulator::handle_fill(CacheLine* blk, addr_t addr) {
  addr_t block_addr = blockAddrOf(addr);
  auto ptr_raw = blk->getRawData<uint8_t>();
  tint_t latency = 0;
  for (size_t i = 0; i < lineBytes_; i += sizeof(word_t)) {
    latency += pmem_read(block_addr + i, (word_t*)(ptr_raw + i), i == 0);
  }
  blk->setTag(tagOf(addr));
  blk->setValid();
  blk->stamp = curr_tick();
  // SIMPRINTFN(CACHE, "Cache fill: tag %x", tagOf(addr));
  return latency;
}

CacheLine*
CacheSimulator::access(addr_t addr) {
  ++stats_.accesses;
  addr_t tag = tagOf(addr);
  size_t si = setIndexOf(addr);
  auto& set = setsArr_.at(si);

  // find hit in this set
  for (size_t i = 0; i < set.size(); ++i) {
    auto& l = set.at(i);
    if (l.isValid() && l.getTag() == tag) {
      l.stamp = curr_tick();
      ++stats_.hits;
      SIMPRINTFN(CACHE, "Cache hit : tag %x set %lu", tag, si);
      return &l;
    }
  }

  // miss: replace LRU
  ++stats_.misses;
  auto it = std::min_element(set.begin(), set.end(),
                             [](const CacheLine& a, const CacheLine& b) {
                               return a.stamp < b.stamp;
                             });
  SIMPRINTFN(CACHE, "Cache miss: tag %x set %lu repl tag %x", tag, si,
             it->getTag());
  it->invalidate();
  return &(*it);
}

bool
CacheSimulator::handle_prefetch(addr_t addr, tick_t stamp) {
  // auto maybe = prefetcher_.probe(addr, stamp);
  // if (!maybe.has_value())
  //   return false;
  // addr_t paddr = *maybe;
  // // bring into cache but do not count as access
  // addr_t tag = tagOf(paddr);
  // size_t si = setIndexOf(paddr);
  // auto& set = setsArr_.at(si);
  // for (size_t i = 0; i < set.size(); ++i) {
  //   auto& l = set.at(i);
  //   if (l.isValid() && l.getTag() == tag) {
  //     l.stamp = stamp;
  //     return true;
  //   }
  // }
  // auto it = std::min_element(set.begin(), set.end(),
  //                            [](const CacheLine& a, const CacheLine& b) {
  //                              return a.stamp < b.stamp;
  //                            });
  // it->tag = tag;
  // it->isValid() = true;
  // it->stamp = stamp;
  return true;
}

CacheSimulator::Stats
CacheSimulator::stats() const {
  return stats_;
}

auto
CacheSimulator::stats_map() const
  -> std::unordered_map<std::string, double> {
  std::unordered_map<std::string, double> m{};
  m["accesses"] = stats_.accesses;
  m["hits"] = stats_.hits;
  m["misses"] = stats_.misses;
  m["hit_rate"] =
    stats_.accesses > 0 ? (double)stats_.hits / stats_.accesses : 0.0;
  m["miss_rate"] = 1.0 - m["hit_rate"];
  return m;
}

auto
CacheSimulator::config_map() const
  -> std::unordered_map<std::string, size_t> {
  std::unordered_map<std::string, size_t> m{};
  m["size"] = size();
  m["assoc"] = assoc();
  m["blkSize"] = blksize();
  m["latency"] = static_cast<size_t>(latency());
  return m;
}

auto
CacheSimulator::reset_stats() -> void {
  stats_ = Stats{0, 0, 0};
}
