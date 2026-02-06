// cacheSim/RamConn.hh
#pragma once

#include "base.hh"
#include "interface.hh"
#include "trace.hh"
#include "types.hh"
#include <cassert>
#include <string>
#include <vector>

namespace cacheSim {
class CacheBase;
}

namespace memSim {
using Cache = cacheSim::CacheBase;
using enum Direction;
using enum MemRWOpt;

// SDRAM arbiter: single channel, larger-id large-priority.
class RAMArbiter : public ClockedObject {
  using trace::MemLoad;
  using trace::MemNone;
  using trace::MemStore;
  using MemTransPtr = std::unique_ptr<MemTrans>;

public:
  // The order in hosts_ matters. The later one has higher priority
  RAMArbiter(const std::string& name, tint_t lat, tint_t bst_lat,
             const std::vector<Cache*>& hosts)
      : ClockedObject(name, nullptr) // TODO:
      , serving_id_{(uint16_t)-1}
      , serving_op_{Read}
      , latency_(lat)
      , burst_latency_(bst_lat)
      , busy_until_(InfTime)
      , hosts_{hosts}
      , reqs_(hosts.size()) {}

  void recv_req(MemTransPtr req);

  // SimObject interface
  json
  config_json() const override {
    json j;
    j["type"] = "RAMArbiter";
    j["latency"] = latency_;
    j["burst_latency"] = burst_latency_;
    j["num_hosts"] = hosts_.size();
    return j;
  }

  json
  stats_json() const override {
    return config_json();
  }

  void
  reset_stats() override {}

  void
  dump_stats(std::ostream& os = std::cout) const override {
    os << name() << " (RAMArbiter)\n";
  }

  // Check when the SDRAM will be free
  tick_t
  next_update() const override {
    return busy_until_;
  }

  void update_impl() override;

private:
  inline tint_t
  lat_of(MemTrans* req) const {
    assert(req->bst_len >= 1);
    return latency_ + (req->bst_len - 1) * burst_latency_;
  }

  // MemTrans* serving_req_;
  uint16_t serving_id_;
  MemRWOpt serving_op_;
  tint_t const latency_;
  tint_t const burst_latency_;
  tick_t busy_until_; // When current access finishes
  std::vector<Cache*> const hosts_;

  using HostRWChannel = std::pair<MemTransPtr, MemTransPtr>;
  std::vector<HostRWChannel> reqs_; // pair<Read, Write>
};

} // namespace memSim
