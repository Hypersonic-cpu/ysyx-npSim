
#include "areaSim/AreaEst.hh"
#include "cacheSim/CacheBase.hh"
#include "cacheSim/RamConn.hh"
#include "defines/debug.hh"
#include "defines/interface.hh"
#include "defines/types.hh"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace cacheSim {
using enum MemRWOpt;
using enum Direction;

addr_t
CacheBase::tagOf(addr_t addr) const {
  return blockAddrOf(addr);
}

size_t
CacheBase::setIndexOf(addr_t addr) const {
  return (addr >> offsetBits_) & (sets_ - 1);
}

size_t
CacheBase::offsetOf(addr_t addr) const {
  return addr & (lineBytes_ - 1);
}

addr_t
CacheBase::blockAddrOf(addr_t addr) const {
  return addr & ~(lineBytes_ - 1);
}

size_t
CacheBase::size() const {
  return sets_ * assoc_ * lineBytes_;
}
size_t
CacheBase::assoc() const {
  return assoc_;
}
size_t
CacheBase::blksize() const {
  return lineBytes_;
}

CacheLine*
CacheBase::select_victim(Set& set) {
  size_t set_idx = static_cast<size_t>(&set - &setsArr_.front());
  for (auto& line : set) {
    if (!line.isValid())
      return &line;
  }
  return repl_policy_->getVictim(set_idx, set);
}

CacheLine*
CacheBase::access(addr_t addr) {
  ++stats.accesses;
  addr_t tag = tagOf(addr);
  size_t si = setIndexOf(addr);
  auto& set = setsArr_.at(si);
  // RTL dCache (FSM-based) latches victimWay in idle using the previous
  // request's reqIdx but the CURRENT cache state, because the next request
  // is only accepted after the previous one fully completes.
  size_t lag_way = 0;
  if (victim_way_lag_) {
    size_t lag_si = lag_set_valid_ ? lag_set_idx_ : si;
    auto& lag_set = setsArr_.at(lag_si);
    bool has_invalid = false;
    for (size_t i = 0; i < lag_set.size(); ++i) {
      if (!lag_set.at(i).isValid()) {
        lag_way = i;
        has_invalid = true;
        break;
      }
    }
    if (!has_invalid) {
      auto* victim = repl_policy_->getVictim(lag_si, lag_set);
      lag_way = static_cast<size_t>(victim - &lag_set.front());
    }
  }

  // find hit in this set
  for (size_t i = 0; i < set.size(); ++i) {
    auto& l = set.at(i);
    if (l.isValid() && l.getTag() == tag) {
      if (victim_way_lag_) {
        lag_set_idx_ = si;
        lag_set_valid_ = true;
      }
      repl_policy_->onHit(si, i, l);
      ++this->stats.hits;
      if (l.is_prefetched && prefetcher_) {
        l.is_prefetched = false;
        prefetcher_->prefetch_useful++;
      }
      return &l;
    }
  }

  // miss: Invoking replacement policy
  ++stats.misses;
  CacheLine* victim = nullptr;
  if (victim_way_lag_) {
    auto way = lag_way % assoc_;
    victim = &set.at(way);
    lag_set_idx_ = si;
    lag_set_valid_ = true;
  } else {
    victim = select_victim(set);
    if (victim_way_lag_) {
      lag_set_idx_ = si;
      lag_set_valid_ = true;
    }
  }
  // Write back by caller. Dirty bit is not cleared so far.
  victim->invalidate();
  return victim;
}

void
CacheBase::handle_fill(CacheLine* blk, addr_t addr,
                       const std::vector<word_t>& ret) {
  blk->activate();
  blk->setTag(tagOf(addr));
  size_t si = setIndexOf(addr);
  auto& set = setsArr_.at(si);
  size_t way = 0;
  bool found = false;
  for (size_t i = 0; i < set.size(); ++i) {
    if (&set.at(i) == blk) {
      way = i;
      found = true;
      break;
    }
  }
  assert(found);
  repl_policy_->onFill(si, way, *blk);
  DPRINTF(Cache, "ReFill @ addr %08x", blk->getTag());
  blk->setVecData(ret);
}

bool
CacheBase::probe(addr_t addr) const {
  addr_t tag = tagOf(addr);
  size_t si = setIndexOf(addr);
  auto& set = setsArr_.at(si);
  for (auto& l : set) {
    if (l.isValid() && l.getTag() == tag)
      return true;
  }
  return false;
}

/** Base class handle_prefetch — not expected to be called directly. */
bool
CacheBase::handle_prefetch(addr_t addr, bool is_hit) {
  assert(false && "Override in subclass");
  return false;
}

/** Pipelined Cache */

json
PipeCache::config_json() const {
  json j;
  j["size"] = size();
  j["assoc"] = assoc();
  j["blkSize"] = blksize();
  j["latency"] = pipe_depth_;
  j["write_back"] = write_back_;
  j["wb_hit_resp_delay"] = wb_hit_resp_delay_;

  json ar;
  size_t line_words = lineBytes_ / sizeof(word_t);
  ar["known_area"] = 575.0 + 264.0 * line_words;

  if (sram_dff_) {
    ar["comb_percent"] = 0.15;
    size_t data_bits = size() * 8;
    size_t idx_bits = floorLog2(sets_);
    size_t tag_bits = 32 - offsetBits_ - idx_bits;
    size_t tagv_per_line = tag_bits + 1;
    size_t total_tagv = sets_ * assoc_ * tagv_per_line;
    ar["timing_bits"] = data_bits + total_tagv;
    ar["cacti_objs"] = json::array();
  } else {
    ar["comb_percent"] = 0.31;
    size_t idx_bits = floorLog2(sets_);
    size_t tag_bits = 32 - offsetBits_ - idx_bits;
    size_t tag_macro_bits = tag_bits * sets_;
    size_t data_macro_bits = (lineBytes_ * 8) * sets_;
    ar["timing_bits"] = (size_t)sets_;
    ar["cacti_objs"] = json::array({
      area::sram_macro("tag_sram", tag_macro_bits),
      area::sram_macro("data_sram", data_macro_bits)
    });
  }

  j["area"] = ar;
  return j;
}

void
PipeCache::handle_hit(const PipePtr& bk, bool immediate) {
  auto is_read = bk->mop == Read;
  word_t& dt =
    is_read ? bk->line->atAligned(offsetOf(bk->addr)) : bk->wrdata;
  blocked_until_ = curr_tick() + 1;
  if (is_read) {
    if (write_back_) {
      if (wb_hit_resp_delay_ == 0) {
        cpu_resp_recv_({bk->addr, dt, cache_id_, Read});
      } else {
        sched_hit_resp_ = {bk->addr, dt, cache_id_, Read};
        sched_hit_time_ = curr_tick() + wb_hit_resp_delay_;
      }
    } else {
      // Immediate response: matches RTL iCache where tagHit drives
      // resp.valid combinationally at C2 (same cycle as tag compare).
      cpu_resp_recv_({bk->addr, dt, cache_id_, Read});
    }
  } else {
    auto mask = CacheBase::strbExtend(bk->wrstrb);
    dt = (~mask & dt) | (mask & bk->wrdata);
    bk->line->setDirty();
    if (write_back_) {
      if (wb_hit_resp_delay_ == 0) {
        cpu_resp_recv_({bk->addr, dt, cache_id_, Write});
      } else {
        sched_hit_resp_ = {bk->addr, dt, cache_id_, Write};
        sched_hit_time_ = curr_tick() + wb_hit_resp_delay_;
      }
    } else {
      cpu_resp_recv_({bk->addr, dt, cache_id_, Write});
    }
  }
  DPRINTF(Cache, "Cache Resp (%s) @ addr %08x data %08x",
          bk->mop == Read ? "Read " : "Write", bk->addr, dt);
}

void
PipeCache::recv_mem_resp(MemTransPtr trans) {
  auto is_read = trans->mop == Read;
  DPRINTF(Cache, "Recv Mem[%s] Resp : length %lu",
          is_read ? "Read " : "Write", trans->data.size());
  if (is_read) {
    assert(r_waiting_);
    r_waiting_ = false;
    handle_fill(pipe_.back()->line, trans->addr, trans->data);
    // RTL fillFinish = RegNext(...): 1 extra blocking cycle after the
    // last beat before willShift can go high.  Total = +2 from last beat.
    // CWF: respond 1 cycle after critical word arrives (not after last beat).
    blocked_until_ = curr_tick() + (cwf_ ? 1 : 2);
  } else {
    assert(w_waiting_);
    w_waiting_ = false;
    if (!write_back_) {
      // Write-through: unblock after SDRAM write completes
      blocked_until_ = curr_tick() + 2;
    }
  }
  // Write-back eviction response: nothing extra needed
}

void
PipeCache::update_impl() {
  // Deliver deferred hit response (C3 word-select delay)
  if (sched_hit_time_ <= curr_tick()) {
    cpu_resp_recv_(sched_hit_resp_);
    sched_hit_time_ = InfTime;
  }

  // NOTE: Memory response must come before cache update
  // is_waiting_ is cleared on mem resp
  // Serve target
  if (const auto& bk = pipe_.back()) {
    if (bk->line == nullptr) {
      save_evict_info(bk->addr);
      bk->line = access(bk->addr);
    }
    assert(!is_replay_ || bk->line->isValid());
    bool was_miss = is_replay_;
    is_replay_ = false;
    if (bk->line->isValid()) {
      handle_hit(bk, was_miss);
      if (bk->mop == Read && !r_waiting_) {
        handle_prefetch(bk->addr, !was_miss);
      }
      pipe_.back().reset();  // Clear processed entry
    } else {
      // Model RTL flowing→memreq state transition: the AXI AR
      // request fires one cycle after the miss is detected.
      if (!pending_fill_req_) {
        pending_fill_req_ = true;
        blocked_until_ = curr_tick() + 1;
        return;
      }
      // Dirty eviction: write-back before fill (RTL: evict→fill)
      if (pending_evict_) {
        if (w_waiting_) {
          // Previous eviction write still pending, wait
          blocked_until_ = curr_tick() + 1;
          return;
        }
        mem_side_->recv_req(std::make_unique<MemTrans>(
          Req, Write, evict_addr_, cache_id_,
          static_cast<uint16_t>(evict_data_.size()),
          std::move(evict_data_)));
        w_waiting_ = true;
        pending_evict_ = false;
        // Wait for eviction to complete before fill
        blocked_until_ = curr_tick() + 1;
        return;
      }
      // Wait for eviction write to complete before sending fill
      if (w_waiting_) {
        blocked_until_ = curr_tick() + 1;
        return;
      }
      pending_fill_req_ = false;
      mem_side_->recv_req(std::make_unique<MemTrans>(
        Req, Read, bk->addr, cache_id_,
        static_cast<uint16_t>(lineBytes_ / sizeof(word_t))));
      r_waiting_ = true;
      is_replay_ = true;
      return;
    }
  }
  // Flush cache, next cycle available
  if (pending_flush_
      && std::all_of(pipe_.begin(), pipe_.end(), [](const PipePtr& p) {
           return p == nullptr;
         })) [[unlikely]] {
    handle_flush();
    return;
  }

  // Shift the pipeline
  for (size_t i = pipe_.size() - 1; i > 0; --i) {
    pipe_[i] = std::move(pipe_[i - 1]);
  }

  // RTL willShift check: after shift, speculatively probe the new back
  // entry's tag.  In RTL the SyncReadMem tag compare is combinational
  // with willShift, so a miss blocks req.ready in the SAME cycle.
  // Without this check npsim detects the miss 1 cycle too late,
  // allowing one extra wrong-path fetch to enter the pipe.
  // Also do access() + pending_fill_req_ here to match RTL timing:
  // RTL detects miss AND starts memreq state in the same cycle (T+1),
  // so the AR fires at T+2.  Without this, npsim would need T+1
  // (willShift block) + T+2 (access+pending) + T+3 (AR) = 1 extra.
  if (pipe_.back() && pipe_.back()->line == nullptr
      && !probe(pipe_.back()->addr)) {
    save_evict_info(pipe_.back()->addr);
    pipe_.back()->line = access(pipe_.back()->addr);
    pending_fill_req_ = true;
    blocked_until_ = curr_tick() + 1;
    return;
  }

  is_shifted_ = true;
  // Notify the CPU-side that cache is available this cycle
  cpu_ack_recv_({cache_id_, Read});
  cpu_ack_recv_({cache_id_, Write});
}

bool
PipeCache::handle_prefetch(addr_t addr, bool is_hit) {
  if (!prefetcher_)
    return false;

  auto maybe = prefetcher_->probe(addr, is_hit);
  if (!maybe.has_value())
    return false;

  addr_t paddr = *maybe;
  // Check if already in cache
  addr_t tag = tagOf(paddr);
  size_t si = setIndexOf(paddr);
  auto& set = setsArr_.at(si);
  for (size_t i = 0; i < set.size(); ++i) {
    if (set.at(i).isValid() && set.at(i).getTag() == tag) {
      return false; // Already cached
    }
  }

  // Fill the line immediately (simplified: no memory latency for prefetch)
  prefetcher_->prefetch_issued++;
  auto* victim = select_victim(set);
  size_t way = static_cast<size_t>(victim - &set.front());
  victim->setTag(tag);
  victim->activate();
  repl_policy_->onFill(si, way, *victim);
  victim->is_prefetched = true;
  DPRINTF(Cache, "Prefetch Fill @ %08x (set %zu)", paddr, si);
  return true;
}

void
PipeCache::read_req(addr_t addr) {
  DPRINTF(Cache, "Recv READ Req @ %u", addr);
  assert(is_shifted_);
  assert(pipe_.front() == nullptr);
  auto req = std::make_unique<CachePipeEntry>(addr, nullptr, Read);
  pipe_.front() = std::move(req);
  blocked_until_ = curr_tick() + 1;
  is_shifted_ = false;
}

void
PipeCache::pollute(addr_t addr) {
  // Direct pollution: if addr misses, evict victim and install a
  // valid line.  Models wrong-path iCache fills that could not be
  // issued through the normal pipeline (iCache was blocked on a miss).
  // iCache is read-only (write-through, no dirty eviction concern).
  if (probe(addr))
    return;  // already cached, no pollution
  auto* line = access(addr); // evict victim, set tag, increment misses
  line->activate();            // mark valid (simulates SDRAM fill)
}

void
PipeCache::write_req(addr_t addr, word_t data, uint8_t mask) {
  DPRINTF(Cache, "Recv WRITE Req @ %08x data=%08x strb=%x", addr, data, mask);
  if (write_back_) {
    // Write-back: route through pipe like a read
    assert(is_shifted_);
    assert(pipe_.front() == nullptr);
    auto req = std::make_unique<CachePipeEntry>(addr, nullptr, Write);
    req->wrdata = data;
    req->wrstrb = mask;
    pipe_.front() = std::move(req);
    blocked_until_ = curr_tick() + 1;
    is_shifted_ = false;
  } else {
    // Write-through, no-allocate
    auto blk = probe(addr) ? access(addr) : nullptr;
    if (blk && blk->isValid()) {
      auto wm = CacheBase::strbExtend(mask);
      auto& w = blk->atAligned(offsetOf(addr));
      w = (~wm & w) | (wm & data);
    }
    mem_side_->recv_req(std::make_unique<MemTrans>(
      Req, Write, addr, cache_id_, static_cast<uint16_t>(1),
      std::vector<word_t>({data}), std::vector<uint8_t>({mask})));
    w_waiting_ = true;
    cpu_resp_recv_({addr, 0, cache_id_, Write});
  }
}

void
PipeCache::flush_all() {
  pending_flush_ = true;
}

void
PipeCache::handle_flush() {
  DPRINTF(Cache, "Flush All");
  for (auto& s : setsArr_) {
    for (auto& l : s) {
      l.invalidate();
    }
  }
  // Clear pipeline
  for (auto& p : pipe_) {
    p.reset();
  }
  is_shifted_ = true;
  is_replay_ = false;
  r_waiting_ = false;
  w_waiting_ = false;
  pending_flush_ = false;
  pending_fill_req_ = false;
  pending_evict_ = false;
  sched_hit_time_ = InfTime;
  blocked_until_ = curr_tick() + 1;
}

void
PipeCache::save_evict_info(addr_t req_addr) {
  pending_evict_ = false;
  if (!write_back_) return;
  size_t si = setIndexOf(req_addr);
  auto& set = setsArr_.at(si);
  // Same LRU victim selection as access()
  auto* it = select_victim(set);
  if (it->isValid() && it->isDirty()) {
    pending_evict_ = true;
    evict_addr_ = it->getTag();  // blockAddrOf — already aligned
    size_t nwords = lineBytes_ / sizeof(word_t);
    evict_data_.resize(nwords);
    for (size_t i = 0; i < nwords; ++i)
      evict_data_[i] = it->atAligned(i * sizeof(word_t));
  }
}

// NoCache implementation - direct memory access without caching
void
NoCache::read_req(addr_t addr) {
  assert(is_ready().first);
  mem_side_->recv_req(
    std::make_unique<MemTrans>(Req, Read, addr, cache_id_, 1));
  r_busy_ = true;
  ++stats.accesses;
  ++stats.misses;
}

void
NoCache::write_req(addr_t addr, word_t data, uint8_t mask) {
  assert(is_ready().second);
  mem_side_->recv_req(std::make_unique<MemTrans>(
    Req, Write, addr, cache_id_, 1, std::vector<word_t>({data}),
    std::vector<uint8_t>({mask})));
  w_busy_ = true;

  ++stats.accesses;
  ++stats.misses;
}

void
NoCache::recv_mem_resp(MemTransPtr trans) {
  auto mop = trans->mop;
  auto& busy = mop == Read ? r_busy_ : w_busy_;
  assert(busy);
  if (mop == Read) {
#if ACTIVE_MODE
    assert(trans->data.size() == 0);
    cpu_resp_recv_({trans->addr, 0xdeadbeef, cache_id_, Read});
#else
    assert(trans->data.size() == 1);
    cpu_resp_recv_({trans->addr, trans->data.at(0), cache_id_, Read});
#endif
  } else {
    cpu_resp_recv_({trans->addr, 0xbadc0de, cache_id_, Write});
  }
  cpu_ack_recv_({cache_id_, mop});
  busy = false;
}

void
NoCache::flush_all() {
  // Should NOT do anything. Do not interrupt the ongoing
  // memory requests
}

void
StoreBuffer::update_impl() {
  // Issue deferred read miss to SDRAM (RTL blocked->ar.fire, 1 cycle)
  if (pending_rmiss_time_ <= curr_tick()) {
    mem_side_->recv_req(
      std::make_unique<MemTrans>(Req, Read, pending_rmiss_addr_, cache_id_,
                                 1));
    pending_rmiss_time_ = InfTime;
  }

  if (sched_r_time_ <= curr_tick()) {
    // Scheduled read response
    DPRINTF(Cache, "Send Cpu[%s] Resp @ %08x", "Read ", sched_r_resp_.addr);
    cpu_resp_recv_(sched_r_resp_);
    sched_r_time_ = InfTime;
    r_busy_ = false;
  }

  if (sched_w_time_ <= curr_tick()) {
    // Invoked before CPU-Req at T+1, so sched_w_time_ will
    // not be overwritten
    DPRINTF(Cache, "Send Cpu[%s] Resp @ %08x", "Write", sched_w_resp_.addr);
    cpu_resp_recv_(sched_w_resp_);
    sched_w_time_ = InfTime;
    if (!fifo_.empty() && !w_busy_) {
      auto& front = fifo_.front();
      mem_side_->recv_req(
        std::make_unique<MemTrans>(Req, Write, front.addr, cache_id_, 1,
                                   std::vector<word_t>({front.data}),
                                   std::vector<uint8_t>({front.mask})));
      w_busy_ = true;
    }
  }
}

void
StoreBuffer::read_req(addr_t addr) {
  auto it =
    std::find_if(fifo_.begin(), fifo_.end(), [addr](StBufEnt s) -> bool {
      return s.addr == addr && s.mask == 0xf;
    });
  DPRINTF(Cache, "Recv Cpu[%s] Req @ %08x : Buffer [%s]", "Read ", addr,
          it == fifo_.end() ? "Miss" : "Hit ");
  stats.accesses++;
  if (it == fifo_.end()) {
    // Buffer miss — defer SDRAM request by 1 cycle (RTL blocked state)
    stats.misses++;
    pending_rmiss_time_ = curr_tick() + 1;
    pending_rmiss_addr_ = addr;
    r_busy_ = true;
  } else {
    stats.hits++;
    // Buffer hit
    // Delay the resp for 1 cycle since we are not sure whether
    // CPU side can handle same-cyc resp or not.
    sched_r_time_ = curr_tick() + 1;
    sched_r_resp_ = {.addr = addr,
                     .data = it->data,
                     .id = cache_id_,
                     .mop = MemRWOpt::Read};
  }
}

void
StoreBuffer::write_req(addr_t addr, word_t data, uint8_t mask) {
  DPRINTF(Cache, "Recv Cpu[%s] Req @ %08x", "Write", addr);
  stats.accesses++;
  if (entries == 0) {
    // Unbuffered: send write directly to SDRAM, stall pipeline
    stats.misses++;
    mem_side_->recv_req(
      std::make_unique<MemTrans>(Req, Write, addr, cache_id_, 1,
                                 std::vector<word_t>({data}),
                                 std::vector<uint8_t>({mask})));
    w_busy_ = true;
  } else {
    assert(fifo_.size() < entries);
    fifo_.emplace_back(StBufEnt{addr, data, mask});
    sched_w_time_ = curr_tick() + 1;
    sched_w_resp_ = {
      .addr = addr, .data = 0xbadc0de, .id = cache_id_, .mop = Write};
  }
}

void
StoreBuffer::recv_mem_resp(MemTransPtr trans) {
  DPRINTF(Cache, "Recv Mem[%s] Resp : length %lu",
          (trans->mop == Read) ? "Read " : "Write", trans->data.size());
  if (trans->mop == Read) {
    CpuTrans temp_resp{};
#ifdef ACTIVE_MODE
    assert(trans->data.size() == 0);
    temp_resp = {.addr = trans->addr,
                 .data = 0xbeef'c0de,
                 .id = cache_id_,
                 .mop = MemRWOpt::Read};
#else
    assert(trans->data.size() == 1);
    temp_resp = {.addr = trans->addr,
                 .data = trans->data.at(0),
                 .id = cache_id_,
                 .mop = MemRWOpt::Read};
#endif
    // Defer CPU response by 1 cycle (RTL rValid register delay)
    sched_r_time_ = curr_tick() + 1;
    sched_r_resp_ = temp_resp;
  } else {
    if (entries == 0) {
      // Unbuffered: schedule CPU write response, unblock pipeline
      sched_w_time_ = curr_tick() + 1;
      sched_w_resp_ = {.addr = trans->addr,
                       .data = 0xbadc0de,
                       .id = cache_id_,
                       .mop = Write};
      w_busy_ = false;
    } else {
      auto& front = fifo_.front();
      assert(front.addr == trans->addr);
      if (fifo_.size() == entries) {
        cpu_ack_recv_(AckTrans{.id = cache_id_, .mop = Write});
        DPRINTF(Cache, "StBuf slot available");
      }
      fifo_.pop_front();
      w_busy_ = false;
      // Drain next buffered write to memory if available
      if (!fifo_.empty()) {
        auto& next = fifo_.front();
        mem_side_->recv_req(std::make_unique<MemTrans>(
          Req, Write, next.addr, cache_id_, 1,
          std::vector<word_t>({next.data}),
          std::vector<uint8_t>({next.mask})));
        w_busy_ = true;
      }
    }
  }
}
} // namespace cacheSim
