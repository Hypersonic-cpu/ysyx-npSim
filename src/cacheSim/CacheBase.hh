// cacheSim/CacheBase.hh
#pragma once

#include "areaSim/AreaEst.hh"
#include "cacheSim/CacheLine.hh"
#include "cacheSim/Prefetcher.hh"
#include "cacheSim/ReplPolicy.hh"
#include "cacheSim/RamConn.hh"
#include "defines/base.hh"
#include "defines/interface.hh"
#include "defines/types.hh"
#include "stats.hpp"

#include <bit>
#include <cassert>
#include <cstddef>
#include <list>
#include <memory>
#include <utility>
#include <vector>

namespace pipeSim {
class Processor;
}

namespace cacheSim {

using MemSide = memSim::RAMArbiter;

class CacheBase : public ClockedObject {

  inline static bool
  isPowerOf2(addr_t x) {
    return (x != 0) && ((x & (x - 1)) == 0);
  }

protected:
  inline static size_t
  floorLog2(size_t x) {
    assert(x > 0);
    return static_cast<size_t>(std::bit_width(x) - 1);
  }

public:
  struct CacheStats : public StatsBase {
    CacheStats(const std::string& parent_name)
        : StatsBase(parent_name) {}
    size_t accesses = 0;
    size_t hits = 0;
    size_t misses = 0;

    double
    hit_rate() const {
      return accesses ? static_cast<double>(hits) / accesses : 0.0;
    }
    double
    miss_rate() const {
      return 1.0 - hit_rate();
    }

    json
    gen_json() const override {
      json j;
      j["accesses"] = accesses;
      j["hits"] = hits;
      j["misses"] = misses;
      j["miss_rate"] = miss_rate();
      return j;
    }

    void
    dump_stats(std::ostream& os = std::cout) const override {
      os << name() << " Stats:\n";
      os << "  Accesses: " << accesses << "\n";
      os << "  Hits: " << hits << "\n";
      os << "  Misses: " << misses << "\n";
      os << "  Miss Rate: " << miss_rate() << "\n";
    }

    void
    reset_stats() override {
      accesses = 0;
      hits = 0;
      misses = 0;
    }
  } stats;

public:
  using CPU = pipeSim::Processor;

  using mrresp_t = void (*)(addr_t addr, const std::vector<word_t>& ret);
  using mwresp_t = void (*)(addr_t addr);

  CacheBase(const std::string& name, CPU* host, size_t size_bytes,
            size_t line_bytes, size_t assoc = 1,
            std::shared_ptr<Prefetcher> prefetcher = nullptr,
            uint16_t cache_id = 0,
            std::unique_ptr<ReplPolicy> repl_policy = nullptr,
            bool victim_way_lag = false)
      : ClockedObject(name, &this->stats)
      , stats(name)
      , lineBytes_(line_bytes)
      , offsetBits_(floorLog2(line_bytes))
      , sets_(size_bytes / (line_bytes * assoc))
      , assoc_(assoc)
      , cache_id_(cache_id)
      , victim_way_lag_{victim_way_lag}
      , lag_set_idx_{0}
      , lag_set_valid_{false}
      , setsArr_(sets_, std::vector<CacheLine>(assoc_, {line_bytes}))
      , prefetcher_(prefetcher)
      , repl_policy_(std::move(repl_policy))
      , cpu_resp_recv_(nullptr)
      , cpu_ack_recv_(nullptr)
      , mem_side_(nullptr) {
    assert(size_bytes % (line_bytes * assoc) == 0);
    assert(sets_ > 1 && isPowerOf2(sets_));
    assert(isPowerOf2(line_bytes));
    if (prefetcher_) {
      prefetcher_->setBlockSize(lineBytes_);
    }
    if (!repl_policy_)
      repl_policy_ = make_repl_policy("plru", assoc_, sets_);
  }

  virtual ~CacheBase() = default;

  uint16_t
  cache_id() const {
    return cache_id_;
  }

  // Read / write channel ready
  virtual auto is_ready() const -> std::pair<bool, bool> = 0;
  virtual void flush_all() = 0;
  virtual void read_req(addr_t addr) = 0;
  virtual void write_req(addr_t addr, word_t data, uint8_t mask) = 0;
  virtual void recv_mem_resp(MemTransPtr trans) = 0;

  // Direct pollution: simulate a WP fetch hitting the cache without
  // going through the pipeline.  If the address misses, evict the
  // victim and install a valid line (modeling the WP fill from SDRAM).
  // Default no-op for NoCache.
  virtual void pollute(addr_t addr) {}

  void
  set_cpu_side_handlers(CpuSideMRespReceiver recv, CpuSideAckReceiver ack) {
    cpu_resp_recv_ = recv;
    cpu_ack_recv_ = ack;
  }

  void
  set_mem_port(MemSide* sdram) {
    mem_side_ = sdram;
  }

protected:
  using Set = ReplPolicy::Set;
  /** Access index and check tag, update LRU stamps.
   * @return Pointer to CacheLine on hit, nullptr on miss
   */
  CacheLine* access(addr_t addr);
  CacheLine* select_victim(Set& set);

  /// Tag lookup without stats side effects. Returns true if addr is
  /// present in the cache.
  bool probe(addr_t addr) const;
  /** Fill the cache line, should be called on mem side
   * response.
   */
  virtual void handle_fill(CacheLine* blk, addr_t addr,
                           const std::vector<word_t>& ret);

  // Trigger prefetch logic; returns whether a prefetch was issued
  virtual bool handle_prefetch(addr_t addr, bool is_hit);

  addr_t tagOf(addr_t addr) const;
  size_t setIndexOf(addr_t addr) const;
  size_t offsetOf(addr_t addr) const;
  addr_t wordAligned(addr_t addr) const;
  addr_t blockAddrOf(addr_t addr) const;

  static word_t
  strbExtend(uint8_t strb) {
    word_t mask = 0;
    mask |= (strb & 0x1) ? 0x000000ff : 0;
    mask |= (strb & 0x2) ? 0x0000ff00 : 0;
    mask |= (strb & 0x4) ? 0x00ff0000 : 0;
    mask |= (strb & 0x8) ? 0xff000000 : 0;
    return mask;
  }

  size_t size() const;
  size_t assoc() const;
  size_t blksize() const;

  size_t const lineBytes_;
  // log2 lineBytes_
  size_t const offsetBits_;
  size_t const sets_;
  size_t const assoc_;
  uint16_t const cache_id_; // 0=ICache, 1=DCache
  bool const victim_way_lag_;
  size_t lag_set_idx_;
  bool lag_set_valid_;

  std::vector<std::vector<CacheLine>> setsArr_;
  std::shared_ptr<Prefetcher> prefetcher_;
  std::unique_ptr<ReplPolicy> repl_policy_;

  CpuSideMRespReceiver cpu_resp_recv_;
  CpuSideAckReceiver cpu_ack_recv_;

  MemSide* mem_side_;
}; // CacheBase

class PipeCache : public CacheBase {
public:
  explicit PipeCache(const std::string& name, CPU* host, size_t pipe_depth,
                     size_t size_bytes, size_t line_bytes, size_t assoc = 1,
                     std::shared_ptr<Prefetcher> prefetcher = nullptr,
                     uint16_t cache_id = 0, bool sram_dff = true,
                     bool write_back = false, bool cwf = false,
                     tick_t wb_hit_resp_delay = 1,
                     std::unique_ptr<ReplPolicy> repl_policy = nullptr)
      : CacheBase(name, host, size_bytes, line_bytes, assoc, prefetcher,
                  cache_id, std::move(repl_policy),
                  /* victim_way_lag */ write_back && assoc > 1)
      , pipe_(pipe_depth)
      , pipe_depth_{pipe_depth}
      , sram_dff_{sram_dff}
      , write_back_{write_back}
      , cwf_{cwf}
      , wb_hit_resp_delay_{wb_hit_resp_delay}
      , r_waiting_{false}
      , w_waiting_{false}
      , is_shifted_{true}
      , is_replay_{false}
      , pending_flush_{false}
      , blocked_until_{0} {}

  auto
  is_ready() const -> std::pair<bool, bool> override {
    auto r = is_shifted_ && !pending_flush_;
    if (write_back_) {
      r = r && !r_waiting_ && !w_waiting_ && !pending_fill_req_
        && !is_replay_ && sched_hit_time_ == InfTime;
    }
    // Write-back: writes go through pipe (same readiness as reads)
    // Write-through: writes block when SDRAM write channel busy
    auto w = write_back_ ? r : (!pending_flush_ && !w_waiting_);
    return {r, w};
  }

  bool handle_prefetch(addr_t addr, bool is_hit) override;
  void read_req(addr_t addr) override;
  void write_req(addr_t addr, word_t data, uint8_t mask) override;
  void recv_mem_resp(MemTransPtr trans) override;
  void pollute(addr_t addr) override;

  void flush_all() override;

  // SimObject interface
  json config_json() const override;

  tick_t
  next_update() const override {
    if (r_waiting_ && !pending_fill_req_)
      return sched_hit_time_;  // may still need to deliver deferred hit
    return std::min(blocked_until_, sched_hit_time_);
  }
  void update_impl() override;

protected:
  // do_update comes before reqs are inserted
  struct CachePipeEntry {
    addr_t addr;
    CacheLine* line;
    MemRWOpt mop;
    uint8_t wrstrb;
    word_t wrdata;
  };
  using PipePtr = std::unique_ptr<CachePipeEntry>;

  void handle_hit(const PipePtr& req, bool immediate = false);
  void handle_flush();
  // Write-back: save victim line's dirty data before access() invalidates
  void save_evict_info(addr_t req_addr);

  std::vector<PipePtr> pipe_;
  size_t pipe_depth_;
  bool sram_dff_;
  bool write_back_;
  bool cwf_;
  tick_t wb_hit_resp_delay_;
  bool r_waiting_;
  bool w_waiting_;
  bool is_shifted_;
  bool is_replay_;
  bool pending_flush_;
  // Model RTL flowing→memreq state: defer memory request by 1 cycle
  bool pending_fill_req_{false};
  // Write-back eviction state (saved before access() invalidates)
  bool pending_evict_{false};
  addr_t evict_addr_{0};
  std::vector<word_t> evict_data_;
  tick_t blocked_until_;
  // Deferred hit response (models RTL C3 word-select stage)
  tick_t sched_hit_time_{InfTime};
  CpuTrans sched_hit_resp_{};
};

/**
 * NoCache: Direct memory access without caching.
 * Inherits from CacheBase but overrides read/write to bypass cache.
 */
class NoCache : public CacheBase {
public:
  explicit NoCache(const std::string& name, uint16_t cache_id = 1)
      : CacheBase(name, 0, 8, 4, 1, nullptr,
                  cache_id, make_repl_policy("plru", 1, 2))
      , r_busy_{false}
      , w_busy_{false} {} // Dummy values for base

  auto
  is_ready() const -> std::pair<bool, bool> override {
    return {!r_busy_, !w_busy_};
  }
  void read_req(addr_t addr) override;
  void write_req(addr_t addr, word_t data, uint8_t mask) override;
  void recv_mem_resp(MemTransPtr trans) override;
  void flush_all() override;

  void
  update_impl() override {}

  tick_t
  next_update() const override {
    return InfTime;
  }

  json
  config_json() const override {
    json j;
    j["type"] = "NoCache";
    j["size"] = 0;
    j["area"] = area::area_json(0.0);
    return j;
  }

protected:
  bool r_busy_;
  bool w_busy_;
};

class StoreBuffer : public CacheBase {
public:
  explicit StoreBuffer(const std::string& name, size_t entries,
                       uint16_t cache_id = 1)
      : CacheBase(name, 0, 8, 4, 1, nullptr,
                  cache_id) // 8B total, 4B line, 1-way = 2 sets
      , r_busy_{false}
      , w_busy_{false}
      , pending_flush_{false}
      , entries{entries}
      , sched_r_time_{InfTime}
      , sched_w_time_{InfTime}
      , sched_r_resp_{}
      , sched_w_resp_{}
      , fifo_{} {}

  auto
  is_ready() const -> std::pair<bool, bool> override {
    if (pending_flush_) [[unlikely]] {
      return {false, false};
    }
    // entries==0: unbuffered writes, stall until SDRAM responds
    bool w_rdy = (entries == 0) ? !w_busy_ : fifo_.size() < entries;
    return {!r_busy_, w_rdy};
  }
  void read_req(addr_t addr) override;
  void write_req(addr_t addr, word_t data, uint8_t mask) override;
  void recv_mem_resp(MemTransPtr trans) override;

  void
  flush_all() override {
    pending_flush_ = true;
  }

  void update_impl() override;

  tick_t
  next_update() const override {
    return std::min({sched_r_time_, sched_w_time_, pending_rmiss_time_});
  }

  json
  config_json() const override {
    json j;
    j["type"] = "StoreBuffer";
    j["entries"] = entries;
    // Each entry: 32-bit addr + 32-bit data + 8-bit mask = 72 bits → DFF
    j["area"] = area::area_json(0.0, entries * 72, 0.3);
    // ar["comb_percent"] = 0.3;
    // ar["known_area"] = 0.0;
    // ar["timing_bits"] = entries * 72;
    // ar["cacti_objs"] = json::array();
     // = ar;
    return j;
  }

  struct StBufEnt {
    addr_t addr;
    word_t data;
    uint8_t mask;
  };

protected:
  bool r_busy_;
  bool w_busy_;
  bool pending_flush_;
  size_t write_remain_;

  std::list<StBufEnt> fifo_;
  // size_t busy_ent;
  const size_t entries;

private:
  tick_t sched_w_time_;
  tick_t sched_r_time_;
  // Deferred SDRAM read request (models RTL blocked→ar transition)
  tick_t pending_rmiss_time_{InfTime};
  addr_t pending_rmiss_addr_{0};
  CpuTrans sched_r_resp_;
  CpuTrans sched_w_resp_;
};
} // namespace cacheSim
