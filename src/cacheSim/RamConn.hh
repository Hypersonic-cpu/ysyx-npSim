// cacheSim/RamConn.hh
#pragma once

#include "areaSim/AreaEst.hh"
#include "cacheSim/RamModel.hh"
#include "defines/base.hh"
#include "defines/interface.hh"
#include "defines/types.hh"
#include "defines/mode_ctrl.hh"
#include <algorithm>
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

class RamDevice {
public:
  explicit RamDevice(std::string name)
      : name_(std::move(name))
      , accesses_(0)
      , reads_(0)
      , writes_(0)
      , total_lat_cyc_(0) {}

  virtual ~RamDevice() = default;

  tint_t
  access(const MemTrans& req, tick_t now) {
    auto lat = latency_impl(req, now);
    ++accesses_;
    if (req.mop == Read)
      ++reads_;
    else
      ++writes_;
    total_lat_cyc_ += lat;
    return lat;
  }

  virtual json
  config_json() const {
    json j;
    j["name"] = name_;
    return j;
  }

  virtual json
  stats_json() const {
    json j = config_json();
    j["accesses"] = accesses_;
    j["reads"] = reads_;
    j["writes"] = writes_;
    j["avg_lat_cyc"] =
      accesses_ ? static_cast<double>(total_lat_cyc_) / accesses_ : 0.0;
    return j;
  }

  virtual void
  reset_stats() {
    accesses_ = 0;
    reads_ = 0;
    writes_ = 0;
    total_lat_cyc_ = 0;
  }

protected:
  virtual tint_t latency_impl(const MemTrans& req, tick_t now) = 0;

private:
  std::string name_;
  size_t accesses_;
  size_t reads_;
  size_t writes_;
  size_t total_lat_cyc_;
};

class ConstLatencyRamDevice final : public RamDevice {
public:
  explicit ConstLatencyRamDevice(const std::string& name, tint_t lat_read,
                                 tint_t lat_write = 0)
      : RamDevice(name)
      , lat_read_(lat_read)
      , lat_write_(lat_write ? lat_write : lat_read) {}

  json
  config_json() const override {
    json j = RamDevice::config_json();
    j["type"] = "ConstLatencyRamDevice";
    j["lat_read_cyc"] = lat_read_;
    j["lat_write_cyc"] = lat_write_;
    return j;
  }

protected:
  tint_t
  latency_impl(const MemTrans& req, tick_t) override {
    return req.mop == Read ? lat_read_ : lat_write_;
  }

private:
  tint_t const lat_read_;
  tint_t const lat_write_;
};

class SdramRamDevice final : public RamDevice {
public:
  SdramRamDevice(const std::string& name, tint_t sdram_lat,
                 tint_t sdram_burst, tint_t axi_ovhd = 0,
                 SdramModel* sdram_model = nullptr)
      : RamDevice(name)
      , sdram_lat_(sdram_lat)
      , sdram_burst_(sdram_burst)
      , axi_ovhd_(axi_ovhd)
      , sdram_model_(sdram_model) {}

  json
  config_json() const override {
    json j = RamDevice::config_json();
    j["type"] = "SdramRamDevice";
    if (sdram_model_) {
      j["sdram_model"] = sdram_model_->config_json();
    } else {
      j["sdram_lat_cyc"] = sdram_lat_;
      j["sdram_burst_cyc"] = sdram_burst_;
      j["axi_ovhd_cyc"] = axi_ovhd_;
    }
    return j;
  }

  json
  stats_json() const override {
    json j = RamDevice::stats_json();
    if (sdram_model_)
      j["sdram_stats"] = sdram_model_->stats_json();
    return j;
  }

  void
  reset_stats() override {
    RamDevice::reset_stats();
    if (sdram_model_)
      sdram_model_->reset_stats();
  }

protected:
  tint_t
  latency_impl(const MemTrans& req, tick_t now) override {
    assert(req.bst_len >= 1);
    if (sdram_model_) {
      return sdram_model_->access(
        req.addr, req.bst_len, req.mop == Write, now, req.id);
    }
    return axi_ovhd_ + sdram_lat_ + (req.bst_len - 1) * sdram_burst_;
  }

private:
  tint_t const sdram_lat_;
  tint_t const sdram_burst_;
  tint_t const axi_ovhd_;
  SdramModel* const sdram_model_;
};

struct AddrMapEntry {
  addr_t lo;
  addr_t hi;
  size_t device_id;
  std::string name;

  bool
  contains(addr_t addr) const {
    return addr >= lo && addr <= hi;
  }
};

// Memory arbiter: separate R/W channels, larger-id higher-priority.
// Matches RTL AXIArbiter with independent read and write arbiters.
// Request latency comes from address-mapped devices (SDRAM/SRAM/other).

class RAMArbiter : public ClockedObject {
  using MemTransPtr = std::unique_ptr<MemTrans>;

public:
  // The order in hosts_ matters. The later one has higher priority.
  RAMArbiter(const std::string& name,
             const std::vector<Cache*>& hosts,
             const std::vector<RamDevice*>& devices,
             const std::vector<AddrMapEntry>& addr_map,
             size_t default_device = 0)
      : ClockedObject(name, nullptr)
      , r_serving_id_{(uint16_t)-1}
      , w_serving_id_{(uint16_t)-1}
      , r_busy_until_(InfTime)
      , w_busy_until_(InfTime)
      , hosts_{hosts}
      , devices_{devices}
      , addr_map_{addr_map}
      , default_device_{default_device}
      , reqs_(hosts.size())
      , host_rd_(hosts.size(), 0)
      , host_wr_(hosts.size(), 0)
      , host_dev_(hosts.size(), std::vector<size_t>(devices.size(), 0)) {
    assert(!hosts_.empty());
    assert(!devices_.empty());
    assert(default_device_ < devices_.size());
  }

  void recv_req(MemTransPtr req);

  // SimObject interface
  json
  config_json() const override {
    json j;
    j["type"] = "RAMArbiter";
    j["num_hosts"] = hosts_.size();
    j["num_devices"] = devices_.size();
    json devs = json::array();
    for (const auto* d : devices_) {
      devs.push_back(d->config_json());
    }
    j["devices"] = std::move(devs);
    json amap = json::array();
    for (const auto& e : addr_map_) {
      json x;
      x["name"] = e.name;
      x["lo"] = e.lo;
      x["hi"] = e.hi;
      x["device_id"] = e.device_id;
      amap.push_back(std::move(x));
    }
    j["addr_map"] = std::move(amap);
    j["default_device"] = default_device_;
    j["area"] = area::area_json(200.0);
    return j;
  }

  json
  stats_json() const override {
    json j = config_json();
    for (size_t i = 0; i < hosts_.size(); ++i) {
      j["host" + std::to_string(i) + "_rd"] = host_rd_[i];
      j["host" + std::to_string(i) + "_wr"] = host_wr_[i];
      for (size_t d = 0; d < devices_.size(); ++d) {
        j["host" + std::to_string(i) + "_dev" + std::to_string(d)] =
          host_dev_[i][d];
      }
    }
    json devs = json::array();
    for (const auto* d : devices_)
      devs.push_back(d->stats_json());
    j["device_stats"] = std::move(devs);
    return j;
  }

  void
  reset_stats() override {
    std::fill(host_rd_.begin(), host_rd_.end(), 0);
    std::fill(host_wr_.begin(), host_wr_.end(), 0);
    for (auto& v : host_dev_)
      std::fill(v.begin(), v.end(), 0);
    for (auto* d : devices_)
      d->reset_stats();
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
  inline size_t
  device_of(addr_t addr) const {
    for (const auto& e : addr_map_) {
      if (e.contains(addr))
        return e.device_id;
    }
    return default_device_;
  }

  inline tint_t
  lat_of(MemTrans* req) {
    auto h = static_cast<size_t>(req->id);
    assert(h < hosts_.size());
    auto did = device_of(req->addr);
    assert(did < devices_.size());
    if (req->mop == Read)
      ++host_rd_[h];
    else
      ++host_wr_[h];
    ++host_dev_[h][did];
    return devices_[did]->access(*req, curr_tick());
  }

  // Read channel state
  uint16_t r_serving_id_;
  // Write channel state
  uint16_t w_serving_id_;

  tick_t r_busy_until_;
  tick_t w_busy_until_;
  std::vector<Cache*> const hosts_;
  std::vector<RamDevice*> const devices_;
  std::vector<AddrMapEntry> const addr_map_;
  size_t const default_device_;

  using HostRWChannel = std::pair<MemTransPtr, MemTransPtr>;
  std::vector<HostRWChannel> reqs_; // pair<Read, Write>

  // Per-host request counters and host->device distribution
  std::vector<size_t> host_rd_;
  std::vector<size_t> host_wr_;
  std::vector<std::vector<size_t>> host_dev_;
};

} // namespace memSim
