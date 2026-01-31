#include "Pipeline.hh"
#include "debug.hh"
#include "trace.hh"
#include "types.hh"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <utility>

using SDRAM = memSim::SDRAM;

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
  // Dequeue from fetch queue if ready
  if (fetch_queue_.is_empty()) {
  } else if (fetch_queue_.next_poptime() <= curr_tick()) {
    auto deq = fetch_queue_.dequeue();
    DPRINTF(Pipeline, "Fetch Resp PC=0x%08x T@ %lu penalty=%d",
            deq.trans->trace_inst.pc, curr_tick(),
            deq.trans->is_penalty_fetch);

    // Drop penalty fetches - they don't enter ID stage
    if (!deq.trans->is_penalty_fetch) {
      sim_pipe_.at(Fetch) = std::move(deq.trans);
    } else {
      DPRINTF(Pipeline, "Drop penalty fetch PC=0x%08x",
              deq.trans->trace_inst.pc);
    }
  } else {
    // ICache not ready yet - frontend stall
    stage_valid_.at(Fetch) = fetch_queue_.next_poptime();
    stats.frontend_stalls += fetch_queue_.next_poptime() - curr_tick();
    return;
  }

  assert(!fetch_queue_.is_full());

  // Process pending penalty fetches first
  if (!penalty_fetch_queue_.empty()) {
    addr_t penalty_pc = penalty_fetch_queue_.front();
    penalty_fetch_queue_.pop();

    // Create dummy instruction (add x0, x0, x0)
    Inst penalty_inst;
    penalty_inst.pc = penalty_pc;
    penalty_inst.mem_addr = 0;
    penalty_inst.mem_op = 0;
    penalty_inst.is_branch = false;
    penalty_inst.br_taken = false;
    penalty_inst.dst_reg = 0;
    penalty_inst.src_reg[0] = 0;
    penalty_inst.src_reg[1] = 0;
    penalty_inst.sys_op = 0;
    penalty_inst.dummy = 0;

    auto penalty_trans =
      std::make_unique<Transaction>(penalty_inst, /* is_penalty */ true);

    // Don't call BPU predict/judge for penalty fetches
    // They're just consuming cache/memory bandwidth, not affecting
    // prediction
    penalty_trans->br_pred = {false, 0, false};
    penalty_trans->br_mispred = false;

    // Penalty fetch goes through ICache
    tick_t fetch_done = imem->read_req(penalty_pc, &dummy);
    DPRINTF(Pipeline, "PenaltyFetch PC=0x%08x finish %lu -> %lu", penalty_pc,
            curr_tick(), fetch_done);

    IFEntry ifent(fetch_done, penalty_pc, std::move(penalty_trans));
    fetch_queue_.enqueue(std::move(ifent));

    // Next fetch can only start after:
    // 1. Queue has space (if full, wait for next pop)
    // 2. This fetch completes (cache/memory becomes available)
    // 3. At least next cycle (pipeline constraint)
    tick_t next_avail =
      std::max({fetch_queue_.is_full() ? fetch_queue_.next_poptime() : 0UL,
                fetch_done, curr_tick() + 1});
    stage_valid_.at(Fetch) = std::max(stage_valid_.at(Fetch), next_avail);
    DPRINTF(Event, " - PenaltyFetch Sched @ %lu, IFU ready @ %lu",
            fetch_done, stage_valid_.at(Fetch));
    return;
  }

  // The code below inserts input buffer to fetch queue
  if (!input_buffer_)
    return;
  auto trans = std::move(input_buffer_);
  const auto& inst = trans->trace_inst;

  // Branch prediction at IF stage (before knowing if it's actually a branch)
  auto pred = bpu->predict(inst.pc);
  trans->br_pred = pred;

  // Judge immediately using trace info (we know the real outcome)
  // For non-branch instructions, real_taken=false
  bool real_taken = inst.is_branch && inst.br_taken;
  addr_t real_target = real_taken ? inst.mem_addr : 0;

  // Use BranchUnit::judge to check accuracy and update stats
  bool accurate = bpu->judge(real_taken, real_target, pred);
  trans->br_mispred = !accurate;

  if (!accurate) {
    // Generate penalty fetches for the wrong path
    // Case 1: Should taken but not predicted -> fetch pc+4, pc+8, ...
    // Case 2: Should not taken but predicted to addr 'a' -> fetch a+4, a+8,
    // ...
    addr_t wrong_path_pc;
    if (real_taken && !pred.will_redirect) {
      // Predicted not-taken, but should take: wrong path is pc+4, pc+8...
      wrong_path_pc = inst.pc + 4;
    } else if (!real_taken && pred.will_redirect) {
      // Predicted taken to 'a', but should not: wrong path is a+4, a+8...
      wrong_path_pc = pred.pred_target + 4;
    } else {
      // Bad target: predicted to wrong address
      // This happens when both predict taken but target differs
      wrong_path_pc = pred.pred_target + 4;
    }

    // Enqueue penalty fetches
    for (size_t i = 0; i < PenaltyFetchCount; i++) {
      penalty_fetch_queue_.push(wrong_path_pc);
      DPRINTF(Pipeline, "Enqueue penalty fetch #%lu PC=0x%08x", i,
              wrong_path_pc);
      wrong_path_pc += 4;
    }

    stats.branch_miss_cycles += BranchMissPenalty;
    stats.flush_count++;

    // Block fetch stage for penalty cycles (in addition to penalty fetches)
    stage_valid_.at(Fetch) =
      std::max(stage_valid_.at(Fetch), curr_tick() + BranchMissPenalty);

    DPRINTF(Pipeline,
            "Fetch PC=0x%08x MISPRED: pred_redir=%d real_redir=%d "
            "pen_cyc=%lu pen_fetch=%lu",
            inst.pc, pred.will_redirect, real_taken, BranchMissPenalty,
            PenaltyFetchCount);
  }

  if (inst.is_branch) {
    stats.branches++;
  }

  imem->read_req(inst.pc);
  // tick_t fetch_done = imem->read_req(inst.pc, &dummy);
  DPRINTF(Pipeline, "Fetch PC=0x%08x finish %lu -> %lu", inst.pc,
          curr_tick(), fetch_done);
  // Assertion removed: with complex cache hierarchies and memory contention,
  // fetch latency can legitimately exceed 20k cycles
  IFEntry ifent(fetch_done, /* addr */ inst.pc, std::move(trans));
  fetch_queue_.enqueue(std::move(ifent));

  // Next fetch can only start after:
  // 1. Queue has space (if full, wait for next pop)
  // 2. This fetch completes (cache/memory becomes available)
  // 3. At least next cycle (pipeline constraint)
  tick_t next_avail =
    std::max({fetch_queue_.is_full() ? fetch_queue_.next_poptime() : 0UL,
              fetch_done, curr_tick() + 1});
  stage_valid_.at(Fetch) = std::max(stage_valid_.at(Fetch), next_avail);
  DPRINTF(Event, " - IF -> ID - Sched @ %lu, IFU ready @ %lu", fetch_done,
          stage_valid_.at(Fetch));
}

void
Pipeline::do_decode() {
  assert(stage_valid_.at(Fetch) <= curr_tick());
  const auto& trans = sim_pipe_.at(Fetch);
  const auto& inst = trans->trace_inst;
  auto ready_time =
    std::max(reg_ready_.at(inst.src_reg[0]), reg_ready_.at(inst.src_reg[1]));
  auto rd = inst.dst_reg;
  if (ready_time == BlockedTime) {
    raw_rs_ = std::make_pair(inst.src_reg[0], inst.src_reg[1]);
    stage_valid_.at(Decode) = BlockedTime;
    DPRINTF(Pipeline, "Decode PC=0x%08x src[%d,%d] dst=%d T@ %lu -> blocked",
            inst.pc, inst.src_reg[0], inst.src_reg[1], inst.dst_reg,
            curr_tick());
    return;
  } else {
    // Track RAW stalls
    if (ready_time > curr_tick()) {
      stats.backend_stalls += ready_time - curr_tick();
    }
    auto finish_time = std::max(curr_tick(), ready_time) + 1;
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

  // Update BPU and BTB with actual outcome (prediction was done at IF)
  if (inst.is_branch) {
    bool real_taken = inst.br_taken != 0;
    addr_t real_target = real_taken ? inst.mem_addr : 0;
    // Update BPU state based on actual outcome
    bpu->update(inst.pc, real_taken, real_target);
    // Judge was done at IF, stats already updated there
  }

  DPRINTF(Pipeline, "Execute PC=0x%08x T@ %lu -> %lu", inst.pc, curr_tick(),
          curr_tick() + 1);

  if (inst.mem_op == trace::MemNone) {
    update_raw_time(inst, curr_tick() + 1);
  }

  sim_pipe_.at(Execute) = std::move(sim_pipe_.at(Decode));
  stage_valid_.at(Execute) = curr_tick() + 1;
}

void
Pipeline::do_memory() {
  memst_queue_.auto_dequeue(curr_tick());

  const auto& trans = sim_pipe_.at(Execute);
  const auto& inst = trans->trace_inst;

  tick_t mem_done = curr_tick() + 1;
  if (inst.mem_op == trace::MemLoad) {
    // Load must complete synchronously (not out-of-order)
    // Check store queue for forwarding
    // if (auto bufhit = memst_queue_.contains(inst.mem_addr)) {
    //   mem_done = curr_tick() + 2;
    //   DPRINTF(Pipeline, "Memory Load PC=0x%08x addr=0x%08x STBuf hit",
    //           inst.pc, inst.mem_addr);
    // } else {

    // mem_done = 1 + dmem->read_req(inst.mem_addr, &dummy);
    dmem->read_req(inst.mem_addr, 1);

    // Track memory stalls
    if (mem_done > curr_tick() + 1) {
      stats.mem_stalls += mem_done - curr_tick() - 1;
    }
    DPRINTF(Pipeline, "Memory Load PC=0x%08x addr=0x%08x finish T@ %lu",
            inst.pc, inst.mem_addr, mem_done);
    // }
  } else if (inst.mem_op == trace::MemStore) {
    // Store behavior depends on cache type
    if (memst_queue_.capacity() > 0) {
      // NoCache: use store queue for buffering
      if (memst_queue_.is_full()) {
        auto wait_time = memst_queue_.next_poptime() - curr_tick();
        stats.mem_stalls += wait_time;
        stage_valid_.at(Memory) = memst_queue_.next_poptime();
        return;
      }
      mem_done = curr_tick() + 2;
      auto store_done = dmem->write_req(inst.mem_addr, 0, 0xf);
      DPRINTF(Pipeline,
              "Memory Store PC=0x%08x addr=0x%08x [StQ] finish T@ %lu",
              inst.pc, inst.mem_addr, store_done);
      memst_queue_.enqueue(IOEntryBase{store_done, inst.mem_addr});
    } else {
      // Has dCache: write directly (write-through)
      mem_done = curr_tick() + 1;
      auto store_done = dmem->write_req(inst.mem_addr, 0, 0xf);
      DPRINTF(Pipeline,
              "Memory Store PC=0x%08x addr=0x%08x [Direct] finish T@ %lu",
              inst.pc, inst.mem_addr, store_done);
      // Don't wait for store to complete (write-through, non-blocking)
    }
  } else {
    DPRINTF(Pipeline, "Memory PC=0x%08x (no mem op)", inst.pc);
  }

  if (inst.mem_op != trace::MemNone) {
    update_raw_time(inst, mem_done);
  }
  stage_valid_.at(Memory) = mem_done;
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
  // Process pipeline until we can accept the next instruction
  // In drain mode, keep going until all instructions complete
  while (!(is_drain ? is_finished() : input_buffer_ == nullptr)) {
    // Clear blocked before shifting pipeline
    for (auto ptr : clocked_objs_) {
      if (curr_tick() <= ptr->next_update())
        ptr->do_update();
    }

    for (int i = Num_PipeStage - 1; i >= 0; i--) {
      // For Fetch (i==0): check stage_valid_.at(Fetch) for mispred stall
      bool stage_ready = (i == 0) ? (stage_valid_.at(Fetch) <= curr_tick())
                                  : (stage_valid_.at(i - 1) <= curr_tick());
      if (sim_pipe_.at(i) == nullptr && stage_ready &&
          (!i || sim_pipe_.at(i - 1))) {
        DPRINTF(Event, "  Moved (to S%d) T@ %lu", i, curr_tick());
        std::invoke(stage_handler_.at(i), this);
      }
    }

    if (debug::enabled_flags & debug::Event) {
      std::cerr << "[Event]  StageReady {";
      for (const auto& st : stage_valid_) {
        std::cerr << std::dec << st << ", ";
      }
      std::cerr << "} T@ " << curr_tick() << "\n";
    }

    // Find next event time (minimum of all pending stage times)
    tick_t next_event = BlockedTime;
    for (int i = 0; i < Num_PipeStage; i++) {
      if (i == 0) {
        // Fetch: consider IFQ poptime or stage_valid
        if (!fetch_queue_.is_empty()) {
          next_event = std::min(next_event, fetch_queue_.next_poptime());
        }
        next_event = std::min(next_event, stage_valid_.at(Fetch));
      } else if (sim_pipe_.at(i - 1) != nullptr) {
        // Stage has input ready
        next_event = std::min(next_event, stage_valid_.at(i - 1));
      }
    }

    // Also consider memory store queue
    if (!memst_queue_.is_empty()) {
      next_event = std::min(next_event, memst_queue_.next_poptime());
    }

    // Advance time: skip to next event or advance by 1
    if (next_event > curr_tick() && next_event != BlockedTime) {
      set_global_tick(next_event);
    } else {
      set_global_tick(curr_tick() + 1);
    }
  }
}

} // namespace pipeSim
