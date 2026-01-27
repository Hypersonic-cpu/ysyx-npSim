#pragma once
#include "../types.hh"
#include "CacheLine.hh"
#include "Prefetcher.hh"
#include "base.hh"
#include "stats.hh"
#include <cassert>
#include <cstddef>
#include <memory>
#include <vector>

#if ACTIVE_MODE
#include "debug.hh"
#else
#define DPRINTF(...) do {} while (0)
#endif

namespace cacheSim {

/**
 * Readonly cache simulator with prefetcher.
 * Support functional mode (in timing mode only indicates hit/miss)
 */
class CacheSimulator : public SimObject {
public:
  // Refactored Stats inner class
  struct CacheStats : public StatsBase {
    CacheStats(const std::string& parent_name)
        : StatsBase(parent_name) {}
    size_t accesses = 0;
    size_t hits = 0;
    size_t misses = 0;

    double
    hit_rate() const {
      return accesses ? static_cast<double>(hits) / accesses : 0.0;
    }
    double
    miss_rate() const {
      return 1.0 - hit_rate();
    }

    json
    gen_json() const override {
      json j;
      j["accesses"] = accesses;
      j["hits"] = hits;
      j["misses"] = misses;
      j["miss_rate"] = miss_rate();
      return j;
    }

    void
    dump_stats(std::ostream& os = std::cout) const override {
      os << name() << " Stats:\n";
      os << "  Accesses: " << accesses << "\n";
      os << "  Hits: " << hits << "\n";
      os << "  Misses: " << misses << "\n";
      os << "  Miss Rate: " << miss_rate() << "\n";
    }

    void
    reset_stats() override {
      accesses = 0;
      hits = 0;
      misses = 0;
    }
  } stats;

  CacheSimulator(const std::string& name, size_t size_bytes,
                 size_t line_bytes, size_t assoc = 1,
                 std::shared_ptr<Prefetcher> prefetcher = nullptr);
  // Trigger prefetch logic; returns whether a prefetch was issued
  bool handle_prefetch(addr_t addr, bool is_hit);

  tint_t read_req(addr_t addr, word_t* ret);
  tint_t write_req(addr_t addr, word_t data, uint8_t mask);
  void flush_all();

  // Legacy stats accessors removed/redirected
  json stats_json() const override;
  json config_json() const override;

  auto reset_stats() -> void override;
  void
  dump_stats(std::ostream& os = std::cout) const override {
    stats.dump_stats(os);
  }

protected:
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
  std::shared_ptr<Prefetcher> prefetcher_;
  addr_t tagOf(addr_t addr) const;
  size_t setIndexOf(addr_t addr) const;
  size_t offsetOf(addr_t addr) const;
  addr_t wordAligned(addr_t addr) const;
  addr_t blockAddrOf(addr_t addr) const;
};

} // namespace cacheSim
