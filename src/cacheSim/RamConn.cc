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
  auto& ent = req->mop == Read ? reqs_.at(id).first : reqs_.at(id).second;
  assert(ent == nullptr);
  req->lat = lat_of(req.get());
  DPRINTF(Mem, "Recv [%s] Req from ID = %d lat %u",
          (req->mop == Read) ? "Read " : "Write", id, req->lat);
  ent = std::move(req);
  if (busy_until_ == InfTime) {
    busy_until_ = curr_tick() + ent->lat;
    serving_id_ = id;
    serving_op_ = ent->mop;
    DPRINTF(Mem, "Scheduled event T@ %lu", busy_until_);
  } else {
    DPRINTF(Mem, "Mem already scheduled T@ %lu", busy_until_);
  }
}

void
RAMArbiter::update_impl() {
  // Response current target
  auto& [rd, wr] = reqs_.at(serving_id_);
  auto& ent = serving_op_ == Read ? rd : wr;
  if (ent->mop == Read) {
#if ACTIVE_MODE
#else
    ent->data.resize(ent->bst_len);
    auto i = 0U;
    for (auto& elem : ent->data) {
      pmem_read(ent->addr + i * sizeof(word_t), &elem);
    }
#endif
  } else {
#if ACTIVE_MODE
#else
    for (auto i = 0; i < ent->bst_len; i++) {
      pmem_write(ent->addr + i * 4, ent->data.at(i), ent->strb.at(i));
    }
#endif
  }
  ent->dir = Resp;
  hosts_.at(serving_id_)->recv_mem_resp(std::move(ent));

  // Find next serve target
  auto it = std::find_if(
    reqs_.rbegin(), reqs_.rend(), [](const HostRWChannel& hst) -> bool {
      return hst.first != nullptr || hst.second != nullptr;
    });
  if (it == reqs_.rend()) {
    DPRINTF(Mem, "No targets to serve");
    // Empty. Do not update anymore
    busy_until_ = InfTime;
    serving_id_ = static_cast<uint16_t>(-1);
  } else {
    // Read prior controller - choose the one that's not None
    auto& nxt = it->first != nullptr ? it->first : it->second;
    busy_until_ = curr_tick() + nxt->lat;
    DPRINTF(Mem, "Picking up [%s] @ addr %08x until T@ %lu",
            nxt->mop == Read ? "Read " : "Write", nxt->addr, busy_until_);
    serving_id_ = nxt->id;
    serving_op_ = nxt->mop;
  }
}

} // namespace memSim
