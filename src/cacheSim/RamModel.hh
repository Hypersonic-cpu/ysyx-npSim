// cacheSim/RamModel.hh
// Approximate SDRAM timing model for SoC mode.
//
// Models the sdram_axi_core controller FSM driving a 32-bit SDRAM
// with 4 banks.  Tracks per-bank open-row state and computes
// row-hit / row-miss / row-conflict latencies in device cycles,
// then scales to CPU cycles via the frequency ratio.
//
// Controller parameters (ysyxSoC sdram_top_axi.v):
//   SDRAM_MHZ=100, COL_W=9, ADDR_W=24, CAS=2, SDRAM_DATA_W=32
//   Auto-precharge disabled; rows stay open until precharge/refresh.
//
// Timing from cycle-accurate FSM trace of sdram_axi_core.v.
// N = burst words.  Per-word cost = 2 dev cycles (READ+READ_WAIT
// for reads, IDLE+WRITE0 for writes).
//
// DELAY state cost: setting delay_r=K in a source state causes
// K cycles in STATE_DELAY before exiting to the target state.
//
// Controller occupancy (IDLE to next IDLE-ready), device cycles:
//   Read  hit:      2*N + 3   (IDLE(1) + N*(READ+READ_WAIT) + CAS DELAY(2))
//   Read  miss:     2*N + 6   (+ ACTIVATE(1) + tRCD DELAY(2))
//   Read  conflict: 2*N + 9   (+ PRECHARGE(1) + tRP DELAY(2))
//   Write hit:      2*N       (N * (IDLE+WRITE0), no flush)
//   Write miss:     2*N + 3   (+ ACTIVATE(1) + tRCD DELAY(2))
//   Write conflict: 2*N + 6   (+ PRECHARGE(1) + tRP DELAY(2))
//
// CPU-visible latency: AXI4DelayerChisel converts controller-domain
// occupancy to CPU cycles (cpu_cycles = dev_cyc * freq_ratio).
// The controller is single-threaded: only one request (read or
// write) is processed at a time (modeled by ctrl_free_at_).
//
// Refresh: controller auto-refreshes every REFRESH_PERIOD dev
// cycles, issuing PRECHARGE-ALL + CMD_REFRESH + tRFC delay.
// This closes all open rows; modeled by global refresh tracking.
//
// Address decomposition (sdram_axi_core.v):
//   bank = addr[11:10]   (2 bits -> 4 banks)
//   row  = addr[24:12]   (13 bits)
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

  // Read base: IDLE(1) + CAS flush DELAY(CAS) = 1 + 2 = 3
  static constexpr int RD_BASE = 1 + CAS; // 3
  // Write base: none (first word goes through IDLE+WRITE0 = PER_WORD)
  static constexpr int WR_BASE = 0;

  explicit SdramModel(int cpu_freq_mhz)
      : freq_ratio_(cpu_freq_mhz / DEV_MHZ) {}

  // Compute access latency in CPU cycles.  Updates bank state.
  // cur_tick: current CPU-domain simulation tick (for refresh).
  tint_t
  access(addr_t addr, int bst_len, bool is_write, tint_t cur_tick) {
    int dev_tick = static_cast<int>(cur_tick / freq_ratio_);

    // Global refresh tracking: close all banks when a refresh fires
    check_refresh(dev_tick);

    // Controller serialization: wait if single-threaded FSM is busy
    int start_dev = std::max(dev_tick, ctrl_free_at_);
    if (start_dev > dev_tick)
      ctrl_stall_ += (start_dev - dev_tick);

    int b = bank_of(addr);
    uint32_t r = row_of(addr);

    // Determine row state and compute overhead
    int extra = 0;
    if (!banks_[b].open) {
      extra = ACT_COST; // row miss: need ACTIVATE
      ++row_miss_;
    } else if (banks_[b].row != r) {
      extra = PRE_COST + ACT_COST; // conflict: PRECHARGE + ACTIVATE
      ++row_conf_;
    } else {
      ++row_hit_;
    }

    int base = is_write ? WR_BASE : RD_BASE;
    int dev_cyc = PER_WORD * bst_len + base + extra;

    ctrl_free_at_ = start_dev + dev_cyc;

    banks_[b].open = true;
    banks_[b].row = r;

    int total_dev = ctrl_free_at_ - dev_tick;
    return static_cast<tint_t>(total_dev * freq_ratio_);
  }

  void
  reset_stats() {
    row_hit_ = row_miss_ = row_conf_ = refresh_count_ = 0;
    ctrl_stall_ = 0;
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

  // Check if any refreshes fired since last_refresh_tick_
  void
  check_refresh(int dev_tick) {
    if (dev_tick < next_refresh_tick_)
      return;
    // At least one refresh occurred; close all banks
    for (auto& bk : banks_)
      bk.open = false;
    int elapsed = dev_tick - next_refresh_tick_;
    int n_refreshes = elapsed / REFRESH_PERIOD + 1;
    next_refresh_tick_ += n_refreshes * REFRESH_PERIOD;
    refresh_count_ += n_refreshes;
  }

  int freq_ratio_;
  int ctrl_free_at_ = 0; // dev tick when controller becomes free
  int next_refresh_tick_ = REFRESH_PERIOD;

  struct Bank {
    bool open = false;
    uint32_t row = 0;
  };
  std::array<Bank, N_BANKS> banks_{};

  size_t row_hit_ = 0;
  size_t row_miss_ = 0;
  size_t row_conf_ = 0;
  size_t refresh_count_ = 0;
  size_t ctrl_stall_ = 0; // total dev cycles spent waiting for ctrl
};

} // namespace memSim
