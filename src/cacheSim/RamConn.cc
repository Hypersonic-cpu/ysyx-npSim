#include "cacheSim/CacheBase.hh"
#include "cacheSim/RamConn.hh"
#include "trace.hh"
#include <algorithm>

namespace memSim {
void
RAMArbiter::recv_req(const MemReq& req) {
  auto id = req.id;
  assert(req.op != MemNone);
  auto& ent = req.op == MemLoad ? reqs_.at(id).first : reqs_.at(id).second;
  assert(ent.op == MemNone);
  ent = req;
  ent.lat = lat_of(req);
  if (busy_until_ == InfTime) {
    busy_until_ = curr_tick() + ent.lat;
    serving_req_ = &ent;
  }
}

void
RAMArbiter::update_impl() {
  // Response current target
  auto& ent = *serving_req_;
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
  auto it = std::find_if(
    reqs_.rbegin(), reqs_.rend(), [](const HostPort& hst) -> bool {
      return hst.first.op != MemNone || hst.second.op != MemNone;
    });
  if (it == reqs_.rend()) {
    // Empty. Do not update anymore
    busy_until_ = InfTime;
  } else {
    // Read prior controller
    auto& nxt = it->first.op == MemNone ? it->first : it->second;
    busy_until_ = curr_tick() + nxt.lat;
    serving_req_ = &nxt;
  }
}

} // namespace memSim
