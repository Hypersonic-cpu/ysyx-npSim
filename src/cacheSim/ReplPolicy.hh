#pragma once

#include "cacheSim/CacheLine.hh"
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace cacheSim {

class ReplPolicy {
public:
  using Set = std::vector<CacheLine>;
  virtual ~ReplPolicy() = default;
  virtual CacheLine* getVictim(const Set& set) = 0;
  virtual void onHit(CacheLine& line) = 0;
  virtual void onFill(CacheLine& line) = 0;
};

class LRUReplPolicy final : public ReplPolicy {
public:
  CacheLine* getVictim(const Set& set) override;
  void onHit(CacheLine& line) override;
  void onFill(CacheLine& line) override;
};

class SRRIPReplPolicy final : public ReplPolicy {
public:
  CacheLine* getVictim(const Set& set) override;
  void onHit(CacheLine& line) override;
  void onFill(CacheLine& line) override;
};

class RoundRobinReplPolicy final : public ReplPolicy {
public:
  explicit RoundRobinReplPolicy(size_t assoc)
      : assoc_(assoc)
      , ptr_(0) {}
  CacheLine* getVictim(const Set& set) override;
  void onHit(CacheLine& line) override;
  void onFill(CacheLine& line) override;

private:
  size_t assoc_;
  size_t ptr_;
};

std::unique_ptr<ReplPolicy> make_repl_policy(const std::string& name,
                                             size_t assoc);

} // namespace cacheSim
