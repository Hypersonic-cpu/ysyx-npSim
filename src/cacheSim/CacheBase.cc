
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

// =========================================================
// Cache Base
// =========================================================

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

/** == Protected == */

CacheLine*
CacheBase::access(addr_t addr) {
  ++stats.accesses;
  addr_t tag = tagOf(addr);
  size_t si = setIndexOf(addr);
  auto& set = setsArr_.at(si);

  // find hit in this set
  for (size_t i = 0; i < set.size(); ++i) {
    auto& l = set.at(i);
    if (l.isValid() && l.getTag() == tag) {
      l.stamp = curr_tick();
      ++this->stats.hits;
      if (l.is_prefetched && prefetcher_) {
        l.is_prefetched = false;
        prefetcher_->prefetch_useful++;
      }
      return &l;
    }
  }

  // miss: replace LRU
  ++stats.misses;
  auto it = std::min_element(set.begin(), set.end(),
                             [](const CacheLine& a, const CacheLine& b) {
                               return a.stamp < b.stamp;
                             });
  // Write back by caller. Dirty bit is not cleared so far.
  it->invalidate();
  return &(*it);
}

void
CacheBase::handle_fill(CacheLine* blk, addr_t addr,
                       const std::vector<word_t>& ret) {
  blk->activate();
  blk->setTag(tagOf(addr));
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

bool
CacheBase::handle_prefetch(addr_t addr, bool is_hit) {
  assert(false);
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
    auto& l = set.at(i);
    if (l.isValid() && l.getTag() == tag) {
      // Already in cache
      return false;
    }
  }

  // Not in cache, insert it
  prefetcher_->prefetch_issued++;
  auto it = std::min_element(set.begin(), set.end(),
                             [](const CacheLine& a, const CacheLine& b) {
                               return a.stamp < b.stamp;
                             });
  it->invalidate();
  it->is_prefetched = true;
  return true;
}

// =========================================================
// Pipelined Cache (Read|Write)
// =========================================================

json
PipeCache::config_json() const {
  json j;
  j["size"] = size();
  j["assoc"] = assoc();
  j["blkSize"] = blksize();
  j["latency"] = pipe_depth_;
  return j;
}

void
PipeCache::handle_hit(const PipePtr& bk) {
  auto is_read = bk->mop == Read;
  word_t& dt =
    is_read ? bk->line->atAligned(offsetOf(bk->addr)) : bk->wrdata;
  blocked_until_ = curr_tick() + 1;
  if (is_read) {
    cpu_resp_recv_({bk->addr, dt, cache_id_, Read});
    //   std::make_unique<MemTrans>(
    // Resp, Read, bk->addr, cache_id_, static_cast<uint16_t>(1),
    // std::vector<word_t>({dt})));
  } else {
    auto mask = CacheBase::strbExtend(bk->wrstrb);
    dt = (~mask & dt) | (mask & bk->wrdata);
    bk->line->setDirty();
    cpu_resp_recv_({bk->addr, dt, cache_id_, Write});
    // cpu_resp_recv_(std::make_unique<MemTrans>(
    //   Resp, Write, bk->addr, cache_id_, static_cast<uint16_t>(1),
    //   std::vector<word_t>({dt})));
  }
  DPRINTF(Cache, "Cache Resp (%s) @ addr %08x data %08x",
          bk->mop == Read ? "Read " : "Write", bk->addr, dt);
}

// MUST be called by memory do_update, before cache->do_update
// Triggering next-cycle response to CPU
// void
// PipeCache::memr_resp(addr_t addr, const std::vector<word_t>& ret) {
//   assert(r_waiting_);
//   handle_fill(pipe_.back()->line, addr, ret);
//   blocked_until_ = curr_tick() + 1;
//   r_waiting_ = false;
// }

// void
// PipeCache::memw_resp(addr_t addr) {
void
PipeCache::recv_mem_resp(MemTransPtr trans) {
  auto is_read = trans->mop == Read;
  DPRINTF(Cache, "Recv Mem[%s] Resp : length %lu",
          is_read ? "Read " : "Write", trans->data.size());
  auto& wait = is_read ? r_waiting_ : w_waiting_;
  assert(wait);
  wait = false;
  if (is_read) {
    handle_fill(pipe_.back()->line, trans->addr, trans->data);
  }
  // Write responses are fire-and-forget (already acked to CPU)
  blocked_until_ = curr_tick() + 1;
}

void
PipeCache::update_impl() {
  // NOTE: Memory response must come before cache update
  // is_waiting_ is cleared on mem resp
  // Serve target
  if (const auto& bk = pipe_.back()) {
    // NOTE: Control whether write back or not using `dirty` but not valid.
    // Is replay -> valid
    assert(!is_replay_ || bk->line->isValid());
    bool was_miss = is_replay_;
    is_replay_ = false;
    if (bk->line->isValid()) {
      handle_hit(bk);
      // Trigger prefetch after access (only reads, and only if no
      // outstanding memory request)
      if (bk->mop == Read && !r_waiting_) {
        handle_prefetch(bk->addr, !was_miss);
      }
    } else {
      mem_side_->recv_req(std::make_unique<MemTrans>(
        Req, Read, bk->addr, cache_id_,
        static_cast<uint16_t>(lineBytes_ / sizeof(word_t))));

      if (bk->line->isDirty()) {
        // TODO: Write back if dirty
      }
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
  auto* victim =
    &*std::min_element(set.begin(), set.end(),
                       [](const CacheLine& a, const CacheLine& b) {
                         return a.stamp < b.stamp;
                       });
  victim->setTag(tag);
  victim->activate();
  victim->is_prefetched = true;
  DPRINTF(Cache, "Prefetch Fill @ %08x (set %zu)", paddr, si);
  return true;
}

void
PipeCache::read_req(addr_t addr) {
  // A single CPU-side port should never issue 2 requests in the same cycle
  // Also not allowed when pipe is not shifted
  DPRINTF(Cache, "Recv READ Req @ %u", addr);
  assert(is_shifted_);
  assert(pipe_.front() == nullptr);
  auto blk = access(addr);
  auto req = std::make_unique<CachePipeEntry>(addr, blk, Read);
  pipe_.front() = std::move(req);
  blocked_until_ = curr_tick() + 1;
  is_shifted_ = false;
}

void
PipeCache::write_req(addr_t addr, word_t data, uint8_t mask) {
  // Write-through, no-allocate for dCache
  // Writes don't go through the pipe — fire-and-forget to SDRAM
  DPRINTF(Cache, "Recv WRITE Req @ %08x data=%08x strb=%x", addr, data, mask);
  auto blk = probe(addr) ? access(addr) : nullptr;
  if (blk && blk->isValid()) {
    auto wm = CacheBase::strbExtend(mask);
    auto& w = blk->atAligned(offsetOf(addr));
    w = (~wm & w) | (wm & data);
  }
  // Write-through: send to SDRAM in background
  mem_side_->recv_req(std::make_unique<MemTrans>(
    Req, Write, addr, cache_id_, static_cast<uint16_t>(1),
    std::vector<word_t>({data}), std::vector<uint8_t>({mask})));
  w_waiting_ = true;
  // Immediately acknowledge write to CPU
  cpu_resp_recv_({addr, 0, cache_id_, Write});
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
  blocked_until_ = curr_tick() + 1;
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
  assert(fifo_.size() < entries);
  fifo_.emplace_back(StBufEnt{addr, data, mask});
  sched_w_time_ = curr_tick() + 1;
  sched_w_resp_ = {
    .addr = addr, .data = 0xbadc0de, .id = cache_id_, .mop = Write};
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
} // namespace cacheSim
