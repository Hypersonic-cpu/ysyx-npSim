// cacheSim/RamConn.hh
#pragma once

#include "areaSim/AreaEst.hh"
#include "defines/base.hh"
#include "defines/interface.hh"
#include "defines/types.hh"
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

// SDRAM arbiter: separate R/W channels, larger-id higher-priority.
// Matches RTL AXIArbiter with independent read and write arbiters.
//
// In SoC mode, latency depends on the address of the request:
// SRAM (0x0f000000-0x0fffffff) → fast, SDRAM (0xa0000000+) → slow.

// Address classification helpers (mirrors rvCore.scala)
inline bool
isSRAM(addr_t addr) {
  return (addr >> 24) == 0x0f;
}
inline bool
isCLINT(addr_t addr) {
  return (addr >> 16) == 0x0200;
}
inline bool
isSDRAM(addr_t addr) {
  auto top = addr >> 28;
  return top == 0xa || top == 0xb;
}

class RAMArbiter : public ClockedObject {
  using MemTransPtr = std::unique_ptr<MemTrans>;

public:
  // The order in hosts_ matters. The later one has higher priority
  RAMArbiter(const std::string& name, tint_t lat, tint_t bst_lat,
             const std::vector<Cache*>& hosts,
             bool soc_mode = false, tint_t sram_lat = 1,
             tint_t sdram_single_lat = 50)
      : ClockedObject(name, nullptr)
      , r_serving_id_{(uint16_t)-1}
      , w_serving_id_{(uint16_t)-1}
      , latency_(lat)
      , burst_latency_(bst_lat)
      , soc_mode_(soc_mode)
      , sram_latency_(sram_lat)
      , sdram_single_latency_(sdram_single_lat)
      , r_busy_until_(InfTime)
      , w_busy_until_(InfTime)
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
    j["soc_mode"] = soc_mode_;
    if (soc_mode_) {
      j["sram_latency"] = sram_latency_;
      j["sdram_single_latency"] = sdram_single_latency_;
    }
    j["area"] = area::comb_only(200.0);
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

  tick_t
  next_update() const override {
    return std::min(r_busy_until_, w_busy_until_);
  }

  void update_impl() override;

private:
  inline tint_t
  lat_of(MemTrans* req) const {
    assert(req->bst_len >= 1);
    if (soc_mode_) {
      // SoC: SRAM/CLINT fast, everything else uses SDRAM timing
      if (isSRAM(req->addr) || isCLINT(req->addr)) {
        return sram_latency_;
      }
      // Single-beat SDRAM (e.g. LSU word access): dedicated latency
      if (req->bst_len == 1) {
        return sdram_single_latency_;
      }
    }
    return latency_ + (req->bst_len - 1) * burst_latency_;
  }

  // Read channel state
  uint16_t r_serving_id_;
  // Write channel state
  uint16_t w_serving_id_;

  tint_t const latency_;
  tint_t const burst_latency_;
  bool const soc_mode_;
  tint_t const sram_latency_;
  tint_t const sdram_single_latency_;
  tick_t r_busy_until_;
  tick_t w_busy_until_;
  std::vector<Cache*> const hosts_;

  using HostRWChannel = std::pair<MemTransPtr, MemTransPtr>;
  std::vector<HostRWChannel> reqs_; // pair<Read, Write>
};

} // namespace memSim
