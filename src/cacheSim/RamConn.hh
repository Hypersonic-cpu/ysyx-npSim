// cacheSim/RamConn.hh
#pragma once

#include "areaSim/AreaEst.hh"
#include "defines/base.hh"
#include "defines/interface.hh"
#include "defines/types.hh"
#include "defines/mode_ctrl.hh"
#include <array>
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
// Memory latency model (all values in CPU clock cycles):
//   SDRAM (host > 0): axi_ovhd + sdram_lat + (burst_len - 1) * sdram_burst
//   SDRAM (host 0):   above + icache_extra  (models iCache-specific fill overhead)
//   SRAM/CLINT:       sram_lat  (SoC mode only)

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
  // The order in hosts_ matters. The later one has higher priority.
  RAMArbiter(const std::string& name,
             tint_t sdram_lat, tint_t sdram_burst,
             const std::vector<Cache*>& hosts,
             tint_t sram_lat = 1,
             tint_t axi_ovhd = 0,
             tint_t icache_extra = 0)
      : ClockedObject(name, nullptr)
      , r_serving_id_{(uint16_t)-1}
      , w_serving_id_{(uint16_t)-1}
      , sdram_lat_(sdram_lat)
      , sdram_burst_(sdram_burst)
      , sram_lat_(sram_lat)
      , axi_ovhd_(axi_ovhd)
      , icache_extra_(icache_extra)
      , r_busy_until_(InfTime)
      , w_busy_until_(InfTime)
      , hosts_{hosts}
      , reqs_(hosts.size())
      , host_sdram_rd_(hosts.size(), 0)
      , host_sdram_wr_(hosts.size(), 0) {}

  void recv_req(MemTransPtr req);

  // SimObject interface
  json
  config_json() const override {
    json j;
    j["type"] = "RAMArbiter";
    j["sdram_lat_cyc"] = sdram_lat_;
    j["sdram_burst_cyc"] = sdram_burst_;
    j["axi_ovhd_cyc"] = axi_ovhd_;
    if (icache_extra_ > 0)
      j["icache_extra_cyc"] = icache_extra_;
    if (g_soc_mode)
      j["sram_lat"] = sram_lat_;
    j["num_hosts"] = hosts_.size();
    j["area"] = area::area_json(200.0);
    return j;
  }

  json
  stats_json() const override {
    json j = config_json();
    for (size_t i = 0; i < hosts_.size(); ++i) {
      j["host" + std::to_string(i) + "_sdram_rd"] =
          host_sdram_rd_[i];
      j["host" + std::to_string(i) + "_sdram_wr"] =
          host_sdram_wr_[i];
    }
    return j;
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
  // Total latency for a memory request (in CPU clock cycles).
  //   SDRAM (host > 0): axi_ovhd + sdram_lat + (burst_len - 1) * sdram_burst
  //   SDRAM (host 0):   above + icache_extra  (per-read extra for iCache)
  //   SRAM/CLINT:       sram_lat  (SoC mode only)
  inline tint_t
  lat_of(MemTrans* req) {
    assert(req->bst_len >= 1);
    if (g_soc_mode && (isSRAM(req->addr) || isCLINT(req->addr)))
      return sram_lat_;
    if (req->id < host_sdram_rd_.size()) {
      if (req->mop == Read)
        ++host_sdram_rd_[req->id];
      else
        ++host_sdram_wr_[req->id];
    }
    tint_t lat = axi_ovhd_ + sdram_lat_
                 + (req->bst_len - 1) * sdram_burst_;
    if (icache_extra_ > 0 && req->id == 0 && req->mop == Read)
      lat += icache_extra_;
    return lat;
  }

  // Read channel state
  uint16_t r_serving_id_;
  // Write channel state
  uint16_t w_serving_id_;

  tint_t const sdram_lat_;
  tint_t const sdram_burst_;
  tint_t const sram_lat_;
  tint_t const axi_ovhd_;
  tint_t const icache_extra_;
  tick_t r_busy_until_;
  tick_t w_busy_until_;
  std::vector<Cache*> const hosts_;

  using HostRWChannel = std::pair<MemTransPtr, MemTransPtr>;
  std::vector<HostRWChannel> reqs_; // pair<Read, Write>

  // Per-host SDRAM access counters
  mutable std::vector<size_t> host_sdram_rd_;
  mutable std::vector<size_t> host_sdram_wr_;
};

} // namespace memSim
