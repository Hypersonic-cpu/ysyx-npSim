#pragma once
#include "areaSim/AreaEst.hh"
#include "defines/types.hh"
#include "defines/base.hh"
#include "stats.hpp"
#include <optional>
#include <string>

namespace cacheSim {

class Prefetcher : public SimObject {
protected:
  size_t blkSize_ = 64;

public:
  Prefetcher(const std::string& name)
      : SimObject(name, nullptr) {}
  virtual ~Prefetcher() = default;

  void
  setBlockSize(size_t size) {
    blkSize_ = size;
  }

  // Called on every cache access (hit or miss)
  // Returns an optional address to prefetch
  virtual std::optional<addr_t> probe(addr_t addr, bool is_hit) = 0;

  // Stats
  size_t prefetch_issued = 0;
  size_t prefetch_useful = 0;

  void
  reset_stats() override {
    prefetch_issued = 0;
    prefetch_useful = 0;
  }

  void
  dump_stats(std::ostream& os = std::cout) const override {
    os << name() << " Stats:\n";
    os << "  Issued: " << prefetch_issued << "\n";
    os << "  Useful: " << prefetch_useful << "\n";
  }

  json
  stats_json() const override {
    json j;
    j["issued"] = prefetch_issued;
    j["useful"] = prefetch_useful;
    return j;
  }

  json
  config_json() const override {
    return json{{"area", area::comb_only(0.0)}};
  }
};

class NextLinePrefetcher : public Prefetcher {
public:
  NextLinePrefetcher(const std::string& prefix)
      : Prefetcher(prefix + "-NextLinePrefetcher") {}
  std::optional<addr_t> probe(addr_t addr, bool is_hit) override;
};

class StridePrefetcher : public Prefetcher {
public:
  StridePrefetcher(const std::string& prefix)
      : Prefetcher(prefix + "-StridePrefetcher") {}
  std::optional<addr_t> probe(addr_t addr, bool is_hit) override;

private:
  addr_t last_addr = 0;
  int32_t last_stride = 0;
};

// Tagged prefetcher: prefetch next line only on miss
class TaggedPrefetcher : public Prefetcher {
public:
  TaggedPrefetcher(const std::string& prefix)
      : Prefetcher(prefix + "-TaggedPrefetcher") {}
  std::optional<addr_t>
  probe(addr_t addr, bool is_hit) override {
    if (!is_hit) {
      return addr + blkSize_;
    }
    return std::nullopt;
  }
};

} // namespace cacheSim
