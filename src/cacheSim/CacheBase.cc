
#include "cacheSim/CacheBase.hh"
#include "cacheSim/RamConn.hh"
#include "debug.hh"
#include "trace.hh"
#include "types.hh"
#include <cassert>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace cacheSim {
using trace::MemLoad;
using trace::MemNone;
using trace::MemStore;

// =========================================================
// Cache Base
// =========================================================

// CacheBase::CacheBase(const std::string& name, size_t size_bytes,
//                                size_t line_bytes, size_t assoc,
//                                std::shared_ptr<Prefetcher> prefetcher,
//                                uint16_t cache_id)

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

// tick_t
// CacheBase::read_req(addr_t addr) {
//   auto blk = access(addr);
//   auto off = offsetOf(addr);
//   assert(blk);
//   if (blk->isValid()) {
//     // hit
//     auto blk_ready = std::max(curr_tick(), blk->ready);
//     *ret = blk->atAligned(off);
//     handle_prefetch(addr, true);
//     DPRINTF(Cache, "Hit: Addr=0x%x Tag=0x%x Set=%lu Ready @ %lu", addr,
//             tagOf(addr), setIndexOf(addr), blk->ready);
//     blk->ready++; // At most serve one per cycle
//     return blk_ready + hitTime_;
//   } else {
//     // TODO: if dirty, write back;
//     DPRINTF(Cache, "Miss: Addr=0x%x Tag=0x%x Set=%lu", addr, tagOf(addr),
//             setIndexOf(addr));
//     tick_t fill_done = handle_fill(blk, addr);
//     *ret = blk->atAligned(off);
//     handle_prefetch(addr, false);
//     return judgeTime_ + fill_done;
//   }
// }

// void
// CacheBase::flush_all() {
//   DPRINTF(Cache, "Flush All");
//   for (auto& s : setsArr_) {
//     for (auto& l : s) {
//       l.invalidate();
//     }
//   }
// }

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
      ++stats.hits;
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

// tick_t
// CacheBase::write_req(addr_t addr, word_t data, uint8_t mask) {
//   auto blk = access(addr);
//   auto off = offsetOf(addr);
//   assert(blk);

//   if (blk->isValid()) {
//     // Hit
//     // We don't actually store data in this sim unless needed, but let's
//     do
//     // it if (mask == 0xF) blk->atAligned(off) = data;
//     handle_prefetch(addr, true);
//     return hitTime_;
//   } else {
//     // Miss - Write Allocate?
//     // Typically Write Allocate: fetch block, then write.
//     DPRINTF(Cache, "Write Miss: Addr=0x%x", addr);
//     tint_t latency = hitTime_ + handle_fill(blk, addr);
//     // blk->atAligned(off) = data;
//     handle_prefetch(addr, false);
//     return latency;
//   }
// }

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
  // handle_fill(&(*it), paddr);
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
  auto is_read = bk->op == trace::MemLoad;
  word_t& dt =
    is_read ? bk->line->atAligned(offsetOf(bk->addr)) : bk->wrdata;
  blocked_until_ = curr_tick() + 1;
  if (is_read) {
    std::invoke(read_resp_handler, bk->addr, dt);
  } else {
    auto mask = CacheBase::strbExtend(bk->wrstrb);
    dt = (~mask & dt) | (mask & bk->wrdata);
    bk->line->setDirty();
    std::invoke(write_resp_handler, bk->addr);
  }
  DPRINTF(Cache, "Cache Resp (%s) @ addr %08x data %08x",
          bk->op == MemLoad ? "Rd" : "Wr", bk->addr, dt);
}

void
PipeCache::memr_resp(addr_t addr, const std::vector<word_t>& ret) {
  assert(r_waiting_);
  handle_fill(pipe_.back()->line, addr, ret);
  blocked_until_ = curr_tick();
  r_waiting_ = false;
}

void
PipeCache::memw_resp(addr_t addr) {
  assert(w_waiting_);
  w_waiting_ = false;
  blocked_until_ = curr_tick();
}

void
PipeCache::update_impl() {
  // NOTE: Memory response must come before cache update
  // is_waiting_ is cleared on mem resp
  // Serve target
  if (const auto& bk = pipe_.back()) {
    // NOTE: Control whether write back or not using `dirty` but not valid.
    assert(!is_replay_ || bk->line->isValid());
    is_replay_ = false;
    // TODO: Write back
    if (bk->line->isValid()) {
      handle_hit(bk);
    } else {
      // Cache miss. Always load
      // TODO: Prevent multiple req before response
      mem_side_->recv_req(memSim::MemReq{
        /* op */ MemLoad,
        /* addr  */ bk->addr,
        /* id */ cache_id_,
        /* bstlen */ static_cast<uint16_t>(lineBytes_ / sizeof(word_t))});
      r_waiting_ = true;
      is_replay_ = true;
      return;
    }
  }

  // Shift the pipeline
  for (size_t i = pipe_.size() - 1; i > 0; --i) {
    pipe_[i] = std::move(pipe_[i - 1]);
  }
  is_shifted_ = true;
}

bool
PipeCache::handle_prefetch(addr_t addr, bool is_hit) {
  // Prefetcher is disabled in this implementation
  return false;
}

void
PipeCache::read_req(addr_t addr) {
  // A single CPU-side port should never issue 2 requests in the same cycle
  // Also not allowed when pipe is not shifted
  assert(is_shifted_);
  assert(pipe_.front() == nullptr);
  auto blk = access(addr);
  auto req =
    std::make_unique<CachePipeEntry>(addr, blk, trace::MemOp::MemLoad);
  pipe_.front() = std::move(req);
  is_shifted_ = false;
}

void
PipeCache::write_req(addr_t addr, word_t data, uint8_t mask) {
  // Instruction cache is readonly
  assert(false && "Instruction cache is readonly");
}

void
PipeCache::flush_all() {
  // FIXME: Wait for any existing requests to finish before flushing
  // Currently this flushes immediately without waiting for pending requests
  // which may cause issues if there are in-flight memory transactions

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
  blocked_until_ = 0;
}

// NoCache implementation - direct memory access without caching
void
NoCache::read_req(addr_t addr) {
  assert(!r_busy_);
  mem_side_->recv_req(memSim::MemReq{/* op */ MemLoad,
                                     /* addr */ addr,
                                     /* id */ cache_id_,
                                     /* bstlen*/ 1});
  r_busy_ = true;
  ++stats.accesses;
  ++stats.misses;
}

void
NoCache::write_req(addr_t addr, word_t data, uint8_t mask) {
  assert(!w_busy_);
  mem_side_->recv_req(memSim::MemReq{/* op */ MemStore,
                                     /* addr */ addr,
                                     /* id */ cache_id_,
                                     /* bstlen*/ 1,
                                     /* data */ {data},
                                     /* strb */ {mask}});
  w_busy_ = true;

  ++stats.accesses;
  ++stats.misses;
}

void NoCache::memr_resp(addr_t addr, const std::vector<word_t>& ret) {
  assert(r_busy_);
  assert(ret.size() == 1);
  std::invoke(read_resp_handler, addr, ret[0]);
  r_busy_ = false;
}

void NoCache::memw_resp(addr_t addr) {
  assert(w_busy_);
  std::invoke(write_resp_handler, addr);
  w_busy_ = false;
}

void NoCache::flush_all() {
  // Should NOT do anything. Do not interrupt the ongoing
  // memory requests
}

} // namespace cacheSim
