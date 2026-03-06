// cacheSim/RamConn.hh
#pragma once

#include "areaSim/AreaEst.hh"
#include "defines/base.hh"
#include "defines/interface.hh"
#include "defines/types.hh"
#include "defines/mode_ctrl.hh"
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
// Memory latency model:
//   SDRAM burst: sdram_lat + (burst_len - 1) * sdram_burst_lat
//   In SoC mode, SRAM/CLINT addresses use sram_lat instead.

// Address classification helpers (mirrors rvCore.scala memory map)
inline bool
isSRAM(addr_t addr) {
  return (addr >> 24) == 0x0f;
}
inline bool
isCLINT(addr_t addr) {
  return (addr >> 16) == 0x0200;
}

class RAMArbiter : public ClockedObject {
  using MemTransPtr = std::unique_ptr<MemTrans>;

public:
  // The order in hosts_ matters. The later one has higher priority
  RAMArbiter(const std::string& name, tint_t sdram_lat,
             tint_t sdram_burst_lat,
             const std::vector<Cache*>& hosts,
             tint_t sram_lat = 1,
             tint_t sdram_ovhd = 0)
      : ClockedObject(name, nullptr)
      , r_serving_id_{(uint16_t)-1}
      , w_serving_id_{(uint16_t)-1}
      , sdram_lat_(sdram_lat)
      , sdram_burst_lat_(sdram_burst_lat)
      , sram_lat_(sram_lat)
      , sdram_ovhd_(sdram_ovhd)
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
    j["sdram_lat"] = sdram_lat_;
    j["sdram_burst_lat"] = sdram_burst_lat_;
    j["sdram_ovhd"] = sdram_ovhd_;
    if (g_soc_mode)
      j["sram_lat"] = sram_lat_;
    j["num_hosts"] = hosts_.size();
    j["area"] = area::area_json(200.0);
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
  // Total latency for a memory request.
  //   Single beat:  sdram_ovhd + sdram_lat
  //   Multi-beat:   sdram_ovhd + sdram_lat + (burst_len - 1) * sdram_burst_lat
  //   SRAM/CLINT:   sram_lat  (SoC mode only)
  inline tint_t
  lat_of(MemTrans* req) const {
    assert(req->bst_len >= 1);
    if (g_soc_mode && (isSRAM(req->addr) || isCLINT(req->addr)))
      return sram_lat_;
    return sdram_ovhd_ + sdram_lat_
           + (req->bst_len - 1) * sdram_burst_lat_;
  }

  // Read channel state
  uint16_t r_serving_id_;
  // Write channel state
  uint16_t w_serving_id_;

  tint_t const sdram_lat_;
  tint_t const sdram_burst_lat_;
  tint_t const sram_lat_;
  tint_t const sdram_ovhd_;
  tick_t r_busy_until_;
  tick_t w_busy_until_;
  std::vector<Cache*> const hosts_;

  using HostRWChannel = std::pair<MemTransPtr, MemTransPtr>;
  std::vector<HostRWChannel> reqs_; // pair<Read, Write>
};

} // namespace memSim
