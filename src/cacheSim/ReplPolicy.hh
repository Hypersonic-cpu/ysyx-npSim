#pragma once

#include "cacheSim/CacheLine.hh"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cacheSim {

class ReplPolicy {
public:
  using Set = std::vector<CacheLine>;
  virtual ~ReplPolicy() = default;
  virtual CacheLine* getVictim(size_t set_idx, Set& set) = 0;
  virtual void onHit(size_t set_idx, size_t way_idx, CacheLine& line) = 0;
  virtual void onFill(size_t set_idx, size_t way_idx, CacheLine& line) = 0;
};

class LRUReplPolicy final : public ReplPolicy {
public:
  CacheLine* getVictim(size_t set_idx, Set& set) override;
  void onHit(size_t set_idx, size_t way_idx, CacheLine& line) override;
  void onFill(size_t set_idx, size_t way_idx, CacheLine& line) override;
};

class SRRIPReplPolicy final : public ReplPolicy {
public:
  CacheLine* getVictim(size_t set_idx, Set& set) override;
  void onHit(size_t set_idx, size_t way_idx, CacheLine& line) override;
  void onFill(size_t set_idx, size_t way_idx, CacheLine& line) override;
};

class RoundRobinReplPolicy final : public ReplPolicy {
public:
  RoundRobinReplPolicy(size_t assoc, size_t num_sets)
      : assoc_(assoc)
      , ptr_(num_sets, 0) {}
  CacheLine* getVictim(size_t set_idx, Set& set) override;
  void onHit(size_t set_idx, size_t way_idx, CacheLine& line) override;
  void onFill(size_t set_idx, size_t way_idx, CacheLine& line) override;

private:
  size_t assoc_;
  std::vector<size_t> ptr_;
};

class PLRUReplPolicy final : public ReplPolicy {
public:
  PLRUReplPolicy(size_t assoc, size_t num_sets)
      : assoc_(assoc)
      , bits_(num_sets, 0) {}
  CacheLine* getVictim(size_t set_idx, Set& set) override;
  void onHit(size_t set_idx, size_t way_idx, CacheLine& line) override;
  void onFill(size_t set_idx, size_t way_idx, CacheLine& line) override;

private:
  size_t victim_way(size_t set_idx) const;
  void update_bits(size_t set_idx, size_t way_idx);

  size_t assoc_;
  std::vector<uint8_t> bits_;
};

std::unique_ptr<ReplPolicy> make_repl_policy(const std::string& name,
                                             size_t assoc,
                                             size_t num_sets);

} // namespace cacheSim
