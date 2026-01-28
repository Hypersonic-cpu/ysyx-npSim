#include "Pipeline.hh"
#include "debug.hh"
#include "trace.hh"
#include "types.hh"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
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
Pipeline::do_fetch() {
  // fetch_queue_.auto_dequeue(curr_tick());
  if (fetch_queue_.is_empty()) {
  } else if (fetch_queue_.next_poptime() <= curr_tick()) {
    auto deq = fetch_queue_.dequeue();
    DPRINTF(Pipeline, "Fetch Resp PC=0x%08x T@ %lu",
            deq.trans->trace_inst.pc, curr_tick());
    sim_pipe_.at(Fetch) = std::move(deq.trans);
  } else {
    // Full
    stage_valid_.at(Fetch) = fetch_queue_.next_poptime();
    return;
  }

  assert(!fetch_queue_.is_full());

  // The code below does not insert into sim_pipe_
  if (!input_buffer_)
    return;
  auto trans = std::move(input_buffer_);
  const auto& inst = trans->trace_inst;
  tick_t fetch_done = imem->read_req(inst.pc, &dummy);
  DPRINTF(Pipeline, "Fetch PC=0x%08x finish %lu -> %lu", inst.pc,
          curr_tick(), fetch_done);
  assert(fetch_done - curr_tick() < 20'000U);
  IFEntry ifent(fetch_done, /* addr */ inst.pc, std::move(trans));
  fetch_queue_.enqueue(std::move(ifent));

  // Use current tick for timing if stage is ready, otherwise use stage_ready
  // time
  // auto start_time = std::max(curr_tick(), stage_valid_.at(Fetch));
  // fetch_queue_.enqueue(start_time + if_time, /* not used */ inst.pc);

  stage_valid_.at(Fetch) =
    fetch_queue_.is_full() ? fetch_queue_.next_poptime() : (curr_tick() + 1);
  DPRINTF(Event, " - IF -> ID - Sched @ %lu, IFU ready @ %lu", fetch_done,
          stage_valid_.at(Fetch));
}

void
Pipeline::do_decode() {
  assert(stage_valid_.at(Fetch) <= curr_tick());
  // trans->finish_time = std::max()
  const auto& trans = sim_pipe_.at(Fetch);
  const auto& inst = trans->trace_inst;
  auto ready_time =
    std::max(reg_ready_.at(inst.src_reg[0]), reg_ready_.at(inst.src_reg[1]));
  auto rd = inst.dst_reg;
  if (ready_time == BlockedTime) {
    // trans->next_stage = Decode;
    // trans->finish_time = BlockedTime;
    raw_rs_ = std::make_pair(inst.src_reg[0], inst.src_reg[1]);
    // pending_que_.emplace_back(std::move(trans));
    stage_valid_.at(Decode) = BlockedTime;
    DPRINTF(Pipeline, "Decode PC=0x%08x src[%d,%d] dst=%d T@ %lu -> blocked",
            inst.pc, inst.src_reg[0], inst.src_reg[1], inst.dst_reg,
            curr_tick());
    return;
  } else {
    // Dependencies finish time known
    auto finish_time = std::max(curr_tick(), ready_time) + 1;
    // trans->next_stage = Execute;
    // trans->finish_time = finish_time;
    sim_pipe_.at(Decode) = std::move(sim_pipe_.at(Fetch));
    stage_valid_.at(Decode) = finish_time;
    if (rd) {
      reg_ready_.at(rd) = BlockedTime;
    }
    DPRINTF(Pipeline, "Decode PC=0x%08x src[%d,%d] dst=%d T@ %lu -> %lu",
            inst.pc, inst.src_reg[0], inst.src_reg[1], inst.dst_reg,
            curr_tick(), finish_time);
  }
}

void
Pipeline::do_execute() {
  assert(stage_valid_.at(Decode) <= curr_tick());
  const auto& trans = sim_pipe_.at(Decode);
  const auto& inst = trans->trace_inst;

  DPRINTF(Pipeline, "Execute PC=0x%08x T@ %lu -> %lu", inst.pc, curr_tick(),
          curr_tick() + 1);
  // trans->next_stage = Memory;
  // trans->finish_time = curr_tick() + 1;

  if (inst.mem_op == trace::MemNone) {
    update_raw_time(inst, curr_tick() + 1);
  }

  sim_pipe_.at(Execute) = std::move(sim_pipe_.at(Decode));
  stage_valid_.at(Execute) = curr_tick() + 1;
}

void
Pipeline::do_memory() {
  memst_queue_.auto_dequeue(curr_tick());
  memld_queue_.auto_dequeue(curr_tick());

  const auto& trans = sim_pipe_.at(Execute);
  const auto& inst = trans->trace_inst;

  tick_t mem_done = curr_tick() + 1;
  if (inst.mem_op == trace::MemLoad) {
    if (auto bufhit = memst_queue_.contains(inst.mem_addr)) {
      mem_done = curr_tick() + 2;
      DPRINTF(Pipeline, "Memory Load PC=0x%08x addr=0x%08x STBuf hit",
              inst.pc, inst.mem_addr);
    } else {
      mem_done = 1 + dmem->read_req(inst.mem_addr, &dummy);
      DPRINTF(Pipeline, "Memory Load PC=0x%08x addr=0x%08x finish T@ %lu",
              inst.pc, inst.mem_addr, mem_done);
    }
  } else if (inst.mem_op == trace::MemStore) {
    if (memst_queue_.is_full()) {
      stage_valid_.at(Memory) = memst_queue_.next_poptime();
      // Leave sim pipe (out buffer) empty to block. Out buf of EXU
      // will not be cleared;
      return;
    }
    mem_done = curr_tick() + 2;
    // NOTE: Approximate. In fact we should access dCache when at dequeue.
    auto store_done = dmem->write_req(inst.mem_addr, 0, 0xf);
    DPRINTF(Pipeline,
            "Memory Store PC=0x%08x addr=0x%08x store finish T@ %lu",
            inst.pc, inst.mem_addr, store_done);
    assert(store_done - curr_tick() < 20'000);
    memst_queue_.enqueue(IOEntryBase{store_done, inst.mem_addr});
  } else {
    DPRINTF(Pipeline, "Memory PC=0x%08x (no mem op)", inst.pc);
  }

  // trans->finish_time = curr_tick() + memlat;
  // if (memst_queue_.is_full()) {
  //   auto pop_time = memst_queue_.next_poptime();
  //   assert(pop_time > curr_tick());
  //   stage_valid_.at(Memory) = pop_time;
  // } else {
  //   stage_valid_.at(Memory) = curr_tick() + memlat;
  // }
  // trans->next_stage = WriteBack;

  if (inst.mem_op != trace::MemNone) {
    update_raw_time(inst, mem_done);
  }
  stage_valid_.at(Memory) = mem_done;
  // sim_que_.emplace(std::move(trans));
  sim_pipe_.at(Memory) = std::move(sim_pipe_.at(Execute));
}

void
Pipeline::do_writeback() {
  ongoing_insts_--;
  DPRINTF(Pipeline, "WriteBack PC=0x%08x Remain %lu",
          sim_pipe_.at(Memory)->trace_inst.pc, ongoing_insts_);
  stats.insts++;
  stats.cycles = curr_tick();
  sim_pipe_.at(Memory) = nullptr;
  // ready time === 0
}

// void
// Pipeline::wakeup_pending() {
//   auto removed = false;
//   for (auto& req : pending_que_) {
//     const auto& inst = req->trace_inst;
//     if (stage_avail_time(req->next_stage) == BlockedTime)
//       continue;
//     // if (!req)
//     //   continue;
//     req->finish_time = curr_tick();

// For decode-blocked instructions, check if source registers are ready
// if (req->next_stage == Decode) {
//   const auto& inst = req->trace_inst;
//   auto ready_time = std::max(reg_ready_.at(inst.src_reg[0]),
//                              reg_ready_.at(inst.src_reg[1]));
//   if (ready_time == BlockedTime)
//     continue;
//   req->finish_time = std::max(curr_tick(), ready_time);
// } else {
//   if (stage_avail_time(req->next_stage) == BlockedTime)
//     continue;
//   req->finish_time = curr_tick();
// }

//     sim_que_.emplace(std::move(req));
//     removed = true;
//   }
//   if (removed) {
//     pending_que_.remove_if(
//       [](const TransPtr& ptr) -> bool { return ptr == nullptr; });
//   }
// }

void
Pipeline::update_raw_time(const Inst& inst, tick_t when) {
  if (auto rd = inst.dst_reg) {
    // Stall 1 cycle after finish
    // ID |stall| --> |
    // EX | --> | ^   ^
    //    forward |   | IDU finished
    reg_ready_.at(rd) = when;
  }
  if (raw_rs_.first || raw_rs_.second) {
    auto ready_time =
      std::max(reg_ready_.at(raw_rs_.first), reg_ready_.at(raw_rs_.second));
    if (ready_time < BlockedTime) {
      stage_valid_.at(Decode) = ready_time;
    }
  }
  assert(reg_ready_.at(0) == 0);
}

/**
 * Processing follow the instruction order. Thus start from PCGEN to WB,
 * unlike the backward approach used in gem5.
 */
void
Pipeline::iota_inst(bool is_drain) {
  // assert(!sim_que_.empty());
  // DPRINTF(Event, "Top finish @ %lu next fetch @ %lu",
  //         sim_que_.top()->finish_time, next_fetch());
  // assert(is_drain || sim_que_.top()->finish_time > next_fetch());

  while (is_drain ? !is_finished() : (input_buffer_ != nullptr)) {
    auto last_tick = curr_tick();

    for (int i = Num_PipeStage - 1; i >= 0; i--) {
      // Can output -> next ready, can input -> previous ready.
      // For a blocked stage, although it can output to sim pipe,
      // its subsequent stage cannot process so the sim pipe will not
      // be nullptr until the block time arrived.
      if (sim_pipe_.at(i) == nullptr &&
          (i == 0 || stage_valid_.at(i - 1) <= curr_tick()) &&
          (!i || sim_pipe_.at(i - 1))
          // (i ? sim_pipe_.at(i - 1) : input_buffer_)
      ) {
        DPRINTF(Event, "  Moved (to S%d) T@ %lu", i, curr_tick());
        std::invoke(stage_handler_.at(i), this);
      }
    }

    if (debug::enabled_flags & debug::Event) {
      std::cerr << "[Event]  StageReady {";
      for (const auto& st : stage_valid_) {
        std::cerr << std::dec << st << ", ";
      }
      std::cerr << "}\n";
      std::cerr << "[Event]  StagePointer {";
      for (const auto& st : sim_pipe_) {
        std::cerr << std::dec << st << ", ";
      }
      std::cerr << "}\n";
    }

    set_global_tick(curr_tick() + 1);
  }
}

// while (!sim_que_.empty() &&
//        (is_drain || sim_que_.top()->finish_time <= next_fetch())) {
//   auto trans = std::move(const_cast<TransPtr&>(sim_que_.top()));
//   sim_que_.pop();

//   set_global_tick(trans->finish_time);
//   DPRINTF(Event, " -         dequeue and set tick @ %lu tar %d",
//           curr_tick(), trans->next_stage);

//   auto stage_avail = stage_avail_time(trans->next_stage);
//   if (stage_avail == BlockedTime) {
//     DPRINTF(Event, " -         stage bloked. put into pending que");
//     pending_que_.emplace_back(std::move(trans));
//     continue;
//   } else if (stage_avail > trans->finish_time) {
//     trans->finish_time = stage_avail;
//     DPRINTF(Event, " -         rescheduled to T@ %lu", stage_avail);
//     sim_que_.emplace(std::move(trans));
//     continue;
//   }

//   // Trigger the appropriate stage handler
//   assert(trans->next_stage != Fetch);
//   std::invoke(stage_handler_.at(trans->next_stage), this,
//               std::move(trans));
//   if (last_tick != curr_tick())
//     wakeup_pending();
//   last_tick = curr_tick();

} // namespace pipeSim
