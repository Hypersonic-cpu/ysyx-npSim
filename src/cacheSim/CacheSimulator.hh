#pragma once
#include "../types.hh"
#include "CacheLine.hh"
#include "Prefetcher.hh"
#include "base.hh"
#include <cassert>
#include <climits>
#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

namespace cacheSim {

class CacheSimulator : SimObject {
public:
  struct Stats {
    size_t accesses = 0;
    size_t hits = 0;
    size_t misses = 0;
    double
    hitRate() const {
      return accesses ? static_cast<double>(hits) / accesses : 0.0;
    }
    double
    missRate() const {
      return 1.0 - hitRate();
    }
  };

  CacheSimulator(size_t size_bytes, size_t line_bytes, size_t assoc = 1);
  // Trigger prefetch logic; returns whether a prefetch was issued
  bool handle_prefetch(addr_t addr, tick_t stamp);

  tint_t read_req(addr_t addr, word_t* ret);
  tint_t write_req(addr_t addr, word_t data, uint8_t mask);
  void flush_all();

  Stats stats() const;
  auto stats_map() const -> std::unordered_map<std::string, double> override;
  auto
  config_map() const -> std::unordered_map<std::string, size_t> override;

  auto reset_stats() -> void override;

  size_t size() const;
  size_t assoc() const;
  size_t blksize() const;
  tint_t latency() const;

protected:
  // Access with externally provided stamp, return true on hit
  CacheLine* access(addr_t addr);
  tint_t handle_fill(CacheLine* blk, addr_t addr);

  size_t const lineBytes_;
  // log2 lineBytes_
  size_t const offsetBits_;
  size_t const sets_;
  size_t const assoc_;
  // Set select + tag compare time
  tint_t const hitTime_;

  std::vector<std::vector<CacheLine>> setsArr_;
  Stats stats_;
  std::unique_ptr<Prefetcher> prefetcher_;
  addr_t tagOf(addr_t addr) const;
  size_t setIndexOf(addr_t addr) const;
  size_t offsetOf(addr_t addr) const;
  addr_t wordAligned(addr_t addr) const;
  addr_t blockAddrOf(addr_t addr) const;
};

} // namespace cacheSim
