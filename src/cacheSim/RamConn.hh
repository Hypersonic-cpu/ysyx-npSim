// cacheSim/RamConn.hh
#pragma once

#include "areaSim/AreaEst.hh"
#include "cacheSim/RamModel.hh"
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
//   NPC mode:   axi_ovhd + sdram_lat + (burst_len - 1) * sdram_burst
//   SoC mode:   SdramModel (bank-aware, row-hit/miss/conflict)
//   SRAM/CLINT: sram_lat  (SoC mode only)

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
  // sdram_model: if non-null, used for SDRAM latency (SoC mode).
  RAMArbiter(const std::string& name,
             tint_t sdram_lat, tint_t sdram_burst,
             const std::vector<Cache*>& hosts,
             tint_t sram_lat = 1,
             tint_t axi_ovhd = 0,
             SdramModel* sdram_model = nullptr)
      : ClockedObject(name, nullptr)
      , r_serving_id_{(uint16_t)-1}
      , w_serving_id_{(uint16_t)-1}
      , sdram_lat_(sdram_lat)
      , sdram_burst_(sdram_burst)
      , sram_lat_(sram_lat)
      , axi_ovhd_(axi_ovhd)
      , sdram_model_(sdram_model)
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
    if (sdram_model_) {
      j["sdram_model"] = sdram_model_->config_json();
    } else {
      j["sdram_lat_cyc"] = sdram_lat_;
      j["sdram_burst_cyc"] = sdram_burst_;
      j["axi_ovhd_cyc"] = axi_ovhd_;
    }
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
    if (sdram_model_)
      j["sdram_stats"] = sdram_model_->stats_json();
    return j;
  }

  void
  reset_stats() override {
    if (sdram_model_)
      sdram_model_->reset_stats();
  }

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
  // Compute latency for a memory request (in CPU clock cycles).
  // Called at service time so SDRAM bank state is up-to-date.
  inline tint_t
  lat_of(MemTrans* req) {
    assert(req->bst_len >= 1);
    if ((isSRAM(req->addr) || isCLINT(req->addr)))
      return sram_lat_;
    if (req->id < host_sdram_rd_.size()) {
      if (req->mop == Read)
        ++host_sdram_rd_[req->id];
      else
        ++host_sdram_wr_[req->id];
    }
    if (sdram_model_)
      return sdram_model_->access(req->addr, req->bst_len,
                                  req->mop == Write,
                                  curr_tick());
    return axi_ovhd_ + sdram_lat_
           + (req->bst_len - 1) * sdram_burst_;
  }

  // Read channel state
  uint16_t r_serving_id_;
  // Write channel state
  uint16_t w_serving_id_;

  tint_t const sdram_lat_;
  tint_t const sdram_burst_;
  tint_t const sram_lat_;
  tint_t const axi_ovhd_;
  SdramModel* const sdram_model_;
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
