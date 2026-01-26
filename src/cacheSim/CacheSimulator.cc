
#include "cacheSim/CacheSimulator.hh"
#include "types.hh"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <string>
#include <unordered_map>

using namespace cacheSim;

CacheSimulator::CacheSimulator(const std::string& name, size_t size_bytes,
                               size_t line_bytes, size_t assoc,
                               std::shared_ptr<Prefetcher> prefetcher)
    : SimObject(name)
    , stats(name)
    , lineBytes_(line_bytes)
    , offsetBits_(floorLog2(line_bytes))
    , sets_(size_bytes / (line_bytes * assoc))
    , assoc_(assoc)
    , hitTime_(3)
    , setsArr_(sets_, std::vector<CacheLine>(assoc_, {line_bytes}))
    , prefetcher_(prefetcher) {
  assert(size_bytes % (line_bytes * assoc) == 0);
  assert(sets_ > 1 && isPowerOf2(sets_));
  assert(isPowerOf2(line_bytes));
  if (prefetcher_) {
    prefetcher_->setBlockSize(lineBytes_);
  }
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
    DPRINTF(Cache, "Hit: Addr=0x%x Tag=0x%x Set=%lu", addr, tagOf(addr),
            setIndexOf(addr));
    *ret = blk->atAligned(off);
    handle_prefetch(addr, true);
    return hitTime_;
  } else {
    // TODO: if dirty, write back;
    DPRINTF(Cache, "Miss: Addr=0x%x Tag=0x%x Set=%lu", addr, tagOf(addr),
            setIndexOf(addr));
    tint_t latency = hitTime_ + handle_fill(blk, addr);
    *ret = blk->atAligned(off);
    handle_prefetch(addr, false);
    return latency;
  }
}

void
CacheSimulator::flush_all() {
  DPRINTF(Cache, "Flush All");
  for (auto& s : setsArr_) {
    for (auto& l : s) {
      l.invalidate();
    }
  }
}

tint_t
CacheSimulator::write_req(addr_t addr, word_t data, uint8_t mask) {
  auto blk = access(addr);
  auto off = offsetOf(addr);
  assert(blk);

  if (blk->isValid()) {
    // Hit
    // We don't actually store data in this sim unless needed, but let's do
    // it if (mask == 0xF) blk->atAligned(off) = data;
    handle_prefetch(addr, true);
    return hitTime_;
  } else {
    // Miss - Write Allocate?
    // Typically Write Allocate: fetch block, then write.
    DPRINTF(Cache, "Write Miss: Addr=0x%x", addr);
    tint_t latency = hitTime_ + handle_fill(blk, addr);
    // blk->atAligned(off) = data;
    handle_prefetch(addr, false);
    return latency;
  }
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
  return latency;
}

CacheLine*
CacheSimulator::access(addr_t addr) {
  ++stats.accesses;
  addr_t tag = tagOf(addr);
  size_t si = setIndexOf(addr);
  auto& set = setsArr_.at(si);

  // find hit in this set
  for (size_t i = 0; i < set.size(); ++i) {
    auto& l = set.at(i);
    if (l.isValid() && l.getTag() == tag) {
      l.stamp = curr_tick();
      ++stats.hits;
      if (l.is_prefetched && prefetcher_) {
        l.is_prefetched = false;
        prefetcher_->prefetch_useful++;
      }
      return &l;
    }
  }

  // miss: replace LRU
  ++stats.misses;
  auto it = std::min_element(set.begin(), set.end(),
                             [](const CacheLine& a, const CacheLine& b) {
                               return a.stamp < b.stamp;
                             });
  it->invalidate();
  return &(*it);
}

bool
CacheSimulator::handle_prefetch(addr_t addr, bool is_hit) {
  if (!prefetcher_)
    return false;

  auto maybe = prefetcher_->probe(addr, is_hit);
  if (!maybe.has_value())
    return false;

  addr_t paddr = *maybe;

  // Check if already in cache
  addr_t tag = tagOf(paddr);
  size_t si = setIndexOf(paddr);
  auto& set = setsArr_.at(si);

  for (size_t i = 0; i < set.size(); ++i) {
    auto& l = set.at(i);
    if (l.isValid() && l.getTag() == tag) {
      // Already in cache
      return false;
    }
  }

  // Not in cache, insert it
  prefetcher_->prefetch_issued++;
  auto it = std::min_element(set.begin(), set.end(),
                             [](const CacheLine& a, const CacheLine& b) {
                               return a.stamp < b.stamp;
                             });
  it->invalidate();
  handle_fill(&(*it), paddr);
  it->is_prefetched = true;
  return true;
}

json
CacheSimulator::stats_json() const {
  return stats.gen_json();
}

json
CacheSimulator::config_json() const {
  json j;
  j["size"] = size();
  j["assoc"] = assoc();
  j["blkSize"] = blksize();
  j["latency"] = static_cast<size_t>(latency());
  return j;
}

auto
CacheSimulator::reset_stats() -> void {
  stats.reset_stats();
}
