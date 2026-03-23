// cacheSim/RamModel.hh
// SoC SDRAM timing model derived from ysyxSoC/perip/sdram_axi_core.v.
// It tracks open rows per bank and computes row-hit / miss / conflict
// latency in SDRAM-device cycles before scaling back to CPU cycles.
#pragma once

#include "defines/types.hh"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <array>
#include <cstdint>

namespace memSim {

using json = nlohmann::ordered_json;

class SdramModel {
public:
  // SDRAM device frequency (ysyxSoC uses 100 MHz)
  static constexpr int DEV_MHZ = 100;
  static constexpr int N_HOSTS = 3;

  // Bank / address geometry
  static constexpr int N_BANKS = 4;
  static constexpr int COL_W = 9;
  static constexpr int BANK_W = 2;
  static constexpr int ROW_W = 13; // ADDR_W(24) - COL_W - BANK_W

  // SDRAM timing in device cycles (100 MHz, 10 ns per cycle)
  static constexpr int T_RCD = 2; // ACTIVATE -> READ/WRITE delay
  static constexpr int T_RP = 2;  // PRECHARGE delay
  static constexpr int CAS = 2;   // CAS latency (MODE_REG)
  static constexpr int T_RFC = 6; // ceil(60ns / 10ns)

  // Refresh: (64000*100)/2^13 - 1 = 780 -> period = 781 dev cycles.
  static constexpr int REFRESH_PERIOD = 781;

  // Per-word cost: 2 dev cycles (READ+READ_WAIT or IDLE+WRITE0)
  static constexpr int PER_WORD = 2;

  // ACTIVATE: 1 cycle + DELAY(T_RCD) = 1 + 2 = 3
  static constexpr int ACT_COST = 1 + T_RCD; // 3
  // PRECHARGE: 1 cycle + DELAY(T_RP) = 1 + 2 = 3
  static constexpr int PRE_COST = 1 + T_RP;  // 3

  // Fixed CPU-visible wrapper turns after the burst train.
  static constexpr int RD_BASE = 2;
  static constexpr int WR_BASE = 1;

  explicit SdramModel(int cpu_freq_mhz)
      : freq_ratio_(cpu_freq_mhz / DEV_MHZ) {}

  tint_t
  access(addr_t addr, int bst_len, bool is_write, tint_t cur_tick,
         uint16_t host_id) {
    int dev_tick = static_cast<int>(cur_tick / freq_ratio_);
    size_t host = std::min<size_t>(host_id, N_HOSTS - 1);

    check_refresh(dev_tick);

    int start_dev = std::max(dev_tick, ctrl_free_at_[host]);
    if (start_dev > dev_tick)
      ctrl_stall_ += (start_dev - dev_tick);

    int b = bank_of(addr);
    uint32_t r = row_of(addr);
    auto& bank = banks_[host][b];

    int extra = 0;
    if (!bank.open) {
      extra = ACT_COST;
      ++row_miss_;
    } else if (bank.row != r) {
      extra = PRE_COST + ACT_COST;
      ++row_conf_;
    } else {
      ++row_hit_;
    }

    int base = is_write ? WR_BASE : RD_BASE;
    int dev_cyc = PER_WORD * bst_len + base + extra;

    ctrl_free_at_[host] = start_dev + dev_cyc;

    bank.open = true;
    bank.row = r;

    int total_dev = ctrl_free_at_[host] - dev_tick;
    return static_cast<tint_t>(total_dev * freq_ratio_);
  }

  void
  reset_stats() {
    row_hit_ = row_miss_ = row_conf_ = refresh_count_ = 0;
    ctrl_stall_ = 0;
    ctrl_free_at_.fill(0);
    next_refresh_tick_ = REFRESH_PERIOD;
    for (auto& host_banks : banks_) {
      for (auto& bk : host_banks) {
        bk.open = false;
        bk.row = 0;
      }
    }
  }

  json config_json() const;
  json stats_json() const;

private:
  static int
  bank_of(addr_t a) {
    return (a >> 10) & 0x3;
  }
  static uint32_t
  row_of(addr_t a) {
    return (a >> 12) & 0x1FFF;
  }

  void
  check_refresh(int dev_tick) {
    if (dev_tick < next_refresh_tick_)
      return;
    // At least one refresh occurred; close all banks
    for (auto& host_banks : banks_) {
      for (auto& bk : host_banks)
        bk.open = false;
    }
    int elapsed = dev_tick - next_refresh_tick_;
    int n_refreshes = elapsed / REFRESH_PERIOD + 1;
    next_refresh_tick_ += n_refreshes * REFRESH_PERIOD;
    refresh_count_ += n_refreshes;
  }

  int freq_ratio_;
  std::array<int, N_HOSTS> ctrl_free_at_{};
  int next_refresh_tick_ = REFRESH_PERIOD;

  struct Bank {
    bool open = false;
    uint32_t row = 0;
  };
  std::array<std::array<Bank, N_BANKS>, N_HOSTS> banks_{};

  size_t row_hit_ = 0;
  size_t row_miss_ = 0;
  size_t row_conf_ = 0;
  size_t refresh_count_ = 0;
  size_t ctrl_stall_ = 0;
};

} // namespace memSim
