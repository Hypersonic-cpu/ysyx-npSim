#include "cacheSim/RamConn.hh"
#include "cacheSim/CacheBase.hh"
#include <algorithm>

namespace memSim {
void
RAMArbiter::recv_req(const MemReq& req) {
  auto id = req.id;
  assert(reqs_.at(id).op == MemNone);
  assert(req.op != MemNone);
  auto& ent = reqs_.at(id);
  ent = req;
  ent.lat = lat_of(req);
  if (busy_until_ == InfTime) {
    busy_until_ = curr_tick() + ent.lat;
    serving_id_ = id;
  }
}

void
RAMArbiter::update_impl() {
  // Response current target
  auto ent = reqs_.at(serving_id_);
  if (ent.op == trace::MemLoad) {
    std::vector<word_t> buf(ent.bst_len);
    for (auto i = 0; i < buf.size(); i++) {
      pmem_read(ent.addr + i * sizeof(word_t), &(buf.at(i)));
    }
    hosts_.at(ent.id)->memr_resp(ent.addr, buf);
  } else {
    for (auto i = 0; i < ent.bst_len; i++) {
      pmem_write(ent.addr + i * 4, ent.data.at(i), ent.strb.at(i));
    }
    hosts_.at(ent.id)->memw_resp(ent.addr);
  }

  // Find next serve target
  auto it =
    std::find_if(reqs_.rbegin(), reqs_.rend(), [](MemReq ent) -> bool {
      return ent.op != trace::MemNone;
    });
  if (it == reqs_.rend()) {
    busy_until_ = InfTime;
  } else {
    busy_until_ = curr_tick() + it->lat;
    serving_id_ = it->id;
  }
}

} // namespace memSim
