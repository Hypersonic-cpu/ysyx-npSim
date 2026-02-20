#include "cacheSim/RamConn.hh"
#include "cacheSim/CacheBase.hh"
#include "defines/debug.hh"
#include "defines/interface.hh"
#include "pmem.hh"
#include <algorithm>
#include <cstdint>

namespace memSim {
using enum Direction;
using enum MemRWOpt;

void
RAMArbiter::recv_req(MemTransPtr req) {
  auto id = req->id;
  bool is_read = req->mop == Read;
  auto& ent = is_read ? reqs_.at(id).first : reqs_.at(id).second;
  assert(ent == nullptr);
  req->lat = lat_of(req.get());
  DPRINTF(Mem, "Recv [%s] Req from ID = %d lat %u",
          is_read ? "Read " : "Write", id, req->lat);
  ent = std::move(req);
  if (is_read) {
    if (r_busy_until_ == InfTime) {
      auto start = curr_tick();
      r_busy_until_ = start + ent->lat;
      r_serving_id_ = id;
      DPRINTF(Mem, "R-ch scheduled T@ %lu", r_busy_until_);
    }
  } else {
    if (w_busy_until_ == InfTime) {
      w_busy_until_ = curr_tick() + ent->lat;
      w_serving_id_ = id;
      DPRINTF(Mem, "W-ch scheduled T@ %lu", w_busy_until_);
    }
  }
}

void
RAMArbiter::update_impl() {
  // Handle read channel completion
  if (r_busy_until_ <= curr_tick()
      && r_serving_id_ != static_cast<uint16_t>(-1)) {
    auto& rd = reqs_.at(r_serving_id_).first;
#if ACTIVE_MODE
#else
    rd->data.resize(rd->bst_len);
    for (auto i = 0U; i < rd->data.size(); ++i) {
      pmem_read(rd->addr + i * sizeof(word_t), &rd->data[i]);
    }
#endif
    rd->dir = Resp;
    auto resp_id = r_serving_id_;
    hosts_.at(resp_id)->recv_mem_resp(std::move(rd));

    // Find next read request (higher-id = higher priority)
    r_busy_until_ = InfTime;
    r_serving_id_ = static_cast<uint16_t>(-1);
    for (auto it = reqs_.rbegin(); it != reqs_.rend(); ++it) {
      if (it->first != nullptr) {
        r_serving_id_ = it->first->id;
        auto start = curr_tick();
        r_busy_until_ = start + it->first->lat;
        DPRINTF(Mem, "R-ch picking [Read] @ %08x until T@ %lu",
                it->first->addr, r_busy_until_);
        break;
      }
    }
    if (r_busy_until_ == InfTime) {
      DPRINTF(Mem, "R-ch idle");
    }
  }

  // Handle write channel completion
  if (w_busy_until_ <= curr_tick()
      && w_serving_id_ != static_cast<uint16_t>(-1)) {
    auto& wr = reqs_.at(w_serving_id_).second;
#if ACTIVE_MODE
#else
    for (auto i = 0; i < wr->bst_len; i++) {
      pmem_write(wr->addr + i * 4, wr->data.at(i), wr->strb.at(i));
    }
#endif
    wr->dir = Resp;
    auto resp_id = w_serving_id_;
    hosts_.at(resp_id)->recv_mem_resp(std::move(wr));

    // Find next write request (higher-id = higher priority)
    w_busy_until_ = InfTime;
    w_serving_id_ = static_cast<uint16_t>(-1);
    for (auto it = reqs_.rbegin(); it != reqs_.rend(); ++it) {
      if (it->second != nullptr) {
        w_serving_id_ = it->second->id;
        w_busy_until_ = curr_tick() + it->second->lat;
        DPRINTF(Mem, "W-ch picking [Write] @ %08x until T@ %lu",
                it->second->addr, w_busy_until_);
        break;
      }
    }
    if (w_busy_until_ == InfTime) {
      DPRINTF(Mem, "W-ch idle");
    }
  }
}

} // namespace memSim
