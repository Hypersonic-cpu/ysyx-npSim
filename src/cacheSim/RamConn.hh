// cacheSim/RamConn.hh
#pragma once

#include "base.hh"
#include "shared_types.hh"
#include "trace.hh"
#include "types.hh"
#include <algorithm>
#include <cassert>
#include <string>
#include <vector>

namespace cacheSim {
class CacheBase;
}

namespace memSim {
struct MemReq {
  // read/write
  trace::MemOp op;
  addr_t addr;
  uint16_t id;
  uint16_t bst_len;
  // write
  std::vector<word_t> data;
  std::vector<uint8_t> strb;
  // Set by device
  tint_t lat;
};

using Cache = cacheSim::CacheBase;

// SDRAM arbiter: single channel, larger-id large-priority.
class RAMArbiter : public ClockedObject {
  using trace::MemLoad;
  using trace::MemNone;
  using trace::MemStore;

public:
  // NOTE: the order in hosts_ matters. The later one has higher priority
  RAMArbiter(const std::string& name, tint_t lat, tint_t bst_lat,
             const std::vector<Cache*>& hosts)
      : ClockedObject(name)
      , serving_req_{nullptr}
      , latency_(lat)
      , burst_latency_(bst_lat)
      , busy_until_(0)
      , hosts_{hosts}
      , reqs_(hosts.size(), {{MemNone}, {MemNone}}) {}

  void recv_req(const MemReq& req);

  // Check when the SDRAM will be free
  tick_t
  next_update() const override {
    return busy_until_;
  }

  void update_impl() override;

private:
  inline tint_t
  lat_of(const MemReq& req) const {
    assert(req.bst_len >= 1);
    return latency_ + (req.bst_len - 1) * burst_latency_;
  }

  MemReq* serving_req_;
  tint_t const latency_;
  tint_t const burst_latency_;
  tick_t busy_until_; // When current access finishes
  std::vector<Cache*> const hosts_;
  using HostPort = std::pair<MemReq, MemReq>;
  std::vector<HostPort> reqs_; // pair<Read, Write>
};

} // namespace memSim
