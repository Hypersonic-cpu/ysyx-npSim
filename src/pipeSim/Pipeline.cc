#include "Pipeline.hh"
#include "debug.hh"
#include "trace.hh"
#include "types.hh"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ranges>
#include <utility>

namespace pipeSim {

/**
 * NOTE: In each do_stage handler,
 * - Push unique ptr into sim queue
 * - Update ready time (No such case that do_stage is called but
 *   this stage is blocked due to IO buffer or else)
 * - Mem buffer enqueue
 */

void
Pipeline::do_fetch(Pipeline::TransPtr trans) {
  fetch_queue_.auto_dequeue(curr_tick());

  assert(stage_ready_.at(Fetch) <= curr_tick());
  assert(!fetch_queue_.is_full());
  assert(!trans);

  const auto& inst = trans->trace_inst;
  auto if_time = imem->read_req(inst.pc, &dummy);
  assert(if_time < 20'000U);
  fetch_queue_.enqueue(curr_tick() + if_time, /* not used */ inst.pc);

  if (fetch_queue_.is_full()) {
    auto ins_time = fetch_queue_.next_poptime();
    assert(ins_time > curr_tick());
    // Only for assertion. This method will be called only whne is_full is
    // false
    stage_ready_.at(Fetch) = ins_time;
  } else {
    stage_ready_.at(Fetch) = curr_tick() + 1;
  }
}

void
Pipeline::do_decode(Pipeline::TransPtr trans) {
  assert(stage_ready_.at(Decode) <= curr_tick());
  // trans->finish_time = std::max()
  const auto& inst = trans->trace_inst;
  auto ready_time =
    std::max(reg_ready_.at(inst.src_reg[0]), reg_ready_.at(inst.src_reg[1]));
  auto rd = inst.dst_reg;
  if (ready_time == BlockedTime) {
    trans->next_stage = Decode;
    trans->finish_time = BlockedTime;
    raw_rs_ = std::make_pair(inst.src_reg[0], inst.src_reg[1]);
    pending_que_.emplace_back(std::move(trans));
    return;
  } else {
    auto finish_time = std::max(curr_tick(), ready_time) + 1;
    trans->next_stage = Execute;
    trans->finish_time = finish_time;
    sim_que_.emplace(std::move(trans));
    if (rd) {
      reg_ready_.at(rd) = BlockedTime;
    }
  }
}

void
Pipeline::do_execute(Pipeline::TransPtr trans) {
  assert(stage_ready_.at(Execute) <= curr_tick());
  trans->next_stage = Memory;
  trans->finish_time = curr_tick() + 1;

  if (trans->trace_inst.mem_op == trace::MemNone) {
    update_raw_time(trans);
  }

  sim_que_.emplace(std::move(trans));
  stage_ready_.at(Execute) = curr_tick() + 1;
}

void
Pipeline::do_memory(Pipeline::TransPtr trans) {
  memst_queue_.auto_dequeue(curr_tick());
  memld_queue_.auto_dequeue(curr_tick());

  assert(stage_ready_.at(Memory) <= curr_tick());
  assert(!memst_queue_.is_full());

  const auto& inst = trans->trace_inst;
  auto memlat = 1;
  if (inst.mem_op == trace::MemLoad) {
    if (auto bufhit = memst_queue_.contains(inst.mem_addr)) {
      memlat = 2;
    } else {
      memlat = 1 + dmem->read_req(inst.mem_addr, &dummy);
    }
  } else if (inst.mem_op == trace::MemStore) {
    memlat = 2;
    // NOTE: Approximate. In fact we should access dCache when at dequeue.
    auto storelat = dmem->write_req(inst.mem_addr, 0, 0xf);
    assert(storelat < 20'000);
    memst_queue_.enqueue(curr_tick() + storelat, inst.mem_addr);
  }

  trans->finish_time = curr_tick() + memlat;
  if (memst_queue_.is_full()) {
    auto pop_time = memst_queue_.next_poptime();
    assert(pop_time > curr_tick());
    stage_ready_.at(Memory) = pop_time;
  } else {
    stage_ready_.at(curr_tick()) = memlat;
  }
  trans->next_stage = WriteBack;

  if (inst.mem_op != trace::MemNone) {
    update_raw_time(trans);
  }
  sim_que_.emplace(std::move(trans));
}

void
Pipeline::do_writeback(Pipeline::TransPtr trans) {
  stats.insts++;
  stats.cycles = curr_tick();
  // ready time === 0
}

void
Pipeline::wakeup_pending() {
  auto removed = false;
  for (auto& req : pending_que_) {
    const auto& inst = req->trace_inst;
    if (stage_avail_time(req->next_stage) == BlockedTime)
      continue;
    req->finish_time = curr_tick();
    sim_que_.emplace(std::move(req));
    removed = true;
  }
  if (removed) {
    pending_que_.remove_if(
      [](const TransPtr& ptr) -> bool { return ptr == nullptr; });
  }
}

void
Pipeline::update_raw_time(const TransPtr& trans) {
  const auto& inst = trans->trace_inst;
  if (auto rd = inst.dst_reg) {
    // Stall 1 cycle after finish
    // ID |stall| --> |
    // EX | --> | ^   ^
    //    forward |   | IDU finished
    reg_ready_.at(rd) = trans->finish_time;
  }
  if (raw_rs_.first || raw_rs_.second) {
    auto ready_time =
      std::max(reg_ready_.at(raw_rs_.first), reg_ready_.at(raw_rs_.second));
    if (ready_time < BlockedTime) {
      stage_ready_.at(Decode) = ready_time;
    }
  }
  assert(reg_ready_.at(0) == 0);
}

/**
 * Processing follow the instruction order. Thus start from PCGEN to WB,
 * unlike the backward approach used in gem5.
 */
void
Pipeline::iota_loop() {
  auto this_tick = sim_que_.top()->finish_time;
  set_global_tick(this_tick);

  while (sim_que_.top()->finish_time <= this_tick) {
    auto trans = std::move(const_cast<TransPtr&>(sim_que_.top()));
    sim_que_.pop();

    // Blocked
    // auto ready_time = *std::max_element(
    //   stage_ready_.begin() + static_cast<size_t>(trans->next_stage),
    //   stage_ready_.end());

    auto stage_avail = stage_avail_time(trans->next_stage);
    if (stage_avail == BlockedTime) {
      pending_que_.emplace_back(std::move(stage_avail));
    } else if (stage_avail > trans->finish_time) {
      trans->finish_time = stage_avail;
      sim_que_.emplace(std::move(trans));
      continue;
    }

    // Trigger
    auto time = curr_tick();
    std::invoke(stage_handler_.at(trans->next_stage), std::move(trans));
  }
  wakeup_pending();
}
} // namespace pipeSim
