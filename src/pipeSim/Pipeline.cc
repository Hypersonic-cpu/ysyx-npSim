#include "pipeSim/Pipeline.hh"
#include "defines/debug.hh"
#include "defines/interface.hh"
#include "defines/types.hh"
#include "trace.hh"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <ranges>
#include <utility>

namespace pipeSim {
using trace::MemLoad;
using trace::MemNone;
using trace::MemStore;

Pipeline::Pipeline(const std::string& name, size_t ifq_size, size_t stq_size,
                   BranchUnit* bpu, tick_t br_mis_pen)
    : Processor(name, &this->stats, bpu)
    , stats(name)
    , reg_ready_{}
    , stage_update_{}
    , sim_pipe_{}
    , stage_handler_{&Pipeline::do_fetch_1, &Pipeline::do_decode,
                     &Pipeline::do_execute, &Pipeline::do_memory,
                     &Pipeline::do_writeback}
    , ifq_size_{ifq_size}
    , fetch_inst_queue_{}
    , ongoing_insts_{0}
    , BranchMissPenalty{br_mis_pen} {
  assert(bpu && "BranchUnit must not be null");
}

/**
 * Processing follow the instruction order. Thus start from PCGEN to WB,
 * unlike the backward approach used in gem5.
 */
void
Pipeline::update_impl() {
  pop_pending_ = false; // reset registered toidPtr delay

  // RTL BusConnect Pipeline register: flush arrives 1 cycle after
  // Execute detects the misprediction.
  if (pending_flush_) {
    pending_flush_ = false;
    flush_wrong_path();
    unresolved_branch_ = false;
    if (sim_pipe_.at(Execute) && sim_pipe_.at(Execute)->wait_mem) {
      deferred_br_penalty_ = BranchMissPenalty;
    } else {
      penalty_stall_until_ = curr_tick() + BranchMissPenalty;
    }
  }

  DPRINTF(Event, "Update Pipeline: ");
  for (int i = Num_PipeStage - 1; i >= 0; i--) {
    DPRINTF(Event, " %s : ptr %p, ptr-1 %p, upd %lu",
            StageName.at(i).c_str(), sim_pipe_.at(i).get(),
            (i ? sim_pipe_.at(i - 1) : input_buffer_).get(),
            (int64_t)stage_update_.at(i));
    if (i == static_cast<int>(Decode)) {
      if (sim_pipe_.at(Fetch)) {
        [[maybe_unused]]
        const auto& ifp = sim_pipe_.at(Fetch)->trace_inst;
        DPRINTF(Event, " IDU rs %d, %d time %ld, %ld", ifp.src_reg[0],
                ifp.src_reg[1], reg_ready_.at(ifp.src_reg[0]),
                reg_ready_.at(ifp.src_reg[1]));
      }
    }
    if (sim_pipe_.at(i) == nullptr && (!i || sim_pipe_.at(i - 1) != nullptr)
        && stage_update_.at(i) <= curr_tick()) {
      DPRINTF(Event, "  Moved (%s -> %s) T@ %lu",
              StageName.at(std::max(0, i - 1)).c_str(),
              StageName.at(i).c_str(), curr_tick());
      // stage_update_.at(i) = InfTime;
      std::invoke(stage_handler_.at(i), this);
    }
  }
  DPRINTF(Event, "Try IF Request");
  do_fetch_0();

  calc_sched();
}

void
Pipeline::do_fetch_0() {
  // RTL bufFull = iotaMod(headPtr) === toidPtr.  Checks only the
  // circular buffer (PipeDepth entries).  pop_pending_ models the
  // 1-cycle register delay of toidPtr after io.out.fire: the slot
  // freed by do_fetch_1 doesn't become "available" until next cycle.
  size_t eff_size =
    fetch_inst_queue_.size() + (pop_pending_ ? 1 : 0);
  if (!imem->is_ready().first || eff_size >= ifq_size_) {
    return;
  }
  DPRINTF(Cache, "ICache ready = [%d, %d] InstQue size = %lu",
          imem->is_ready().first, imem->is_ready().second,
          fetch_inst_queue_.size());

  TransPtr candidate = nullptr;

  // End-of-trace draining
  if (is_draining_) [[unlikely]] {
    Inst drain_inst{/* pc        */ 0,
                    /* mem_addr  */ 0,
                    /* mem_op    */ 0,
                    /* is_branch */ false,
                    /* br_taken  */ false,
                    /* dst_reg   */ 0,
                    /* src_reg   */ {0, 0},
                    /* sys_op    */ 0,
                    /* dummy     */ 0};
    candidate = std::make_unique<Transaction>(drain_inst, false, true);
  } else if (unresolved_branch_) {
    // Dynamic wrong-path: keep fetching PC+4 from the mispredicted
    // path until the branch resolves at EX.  The iCache naturally
    // limits the WP count — a miss blocks further WP fetches.
    Inst wp_inst{/* pc        */ wrong_path_pc_,
                 /* mem_addr  */ 0,
                 /* mem_op    */ 0,
                 /* is_branch */ false,
                 /* br_taken  */ false,
                 /* dst_reg   */ 0,
                 /* src_reg   */ {0, 0},
                 /* sys_op    */ 0,
                 /* dummy     */ 0};
    candidate = std::make_unique<Transaction>(wp_inst, true, true);
    wrong_path_pc_ += 4;
    ++wp_this_mispred_;
  } else if (input_buffer_ != nullptr) {
    if (curr_tick() < penalty_stall_until_ || deferred_br_penalty_ > 0) {
      DPRINTF(Pipeline, " IF Penalty Stall (until %lu, now %lu)",
              penalty_stall_until_, curr_tick());
      // Ensure pipeline wakes at penalty expiry even if no in-flight
      // WP entries remain to trigger an async schedule.
      if (penalty_stall_until_ > curr_tick()
          && penalty_stall_until_ != InfTime) {
        schedule(Fetch, penalty_stall_until_);
      }
      return;
    }
    // Penalty recovery ends when first real instruction enters fetch
    if (in_penalty_recovery_) {
      flush_stall_cycles(curr_tick());
      in_penalty_recovery_ = false;
    }
    candidate = std::move(input_buffer_);
    candidate->wait_mem = true;
  } else {
    return;
  }

  const auto& inst = candidate->trace_inst;

  // Branch prediction at IF stage (only for real instructions)
  if (!candidate->is_penalty_fetch) {
    auto pred = bpu->predict(inst.pc);
    auto real_taken = inst.is_branch && inst.br_taken;
    addr_t real_target = real_taken ? inst.mem_addr : 0;
    auto accurate = bpu->judge(real_taken, real_target, pred);
    candidate->br_pred = pred;
    candidate->br_mispred = !accurate;

    if (!accurate) {
      flush_stall_cycles(curr_tick());
      in_penalty_recovery_ = true;
      // Start dynamic wrong-path fetching — the branch will be
      // resolved when it reaches do_execute(), at which point
      // flush_wrong_path() applies the penalty.
      unresolved_branch_ = true;
      if (real_taken && !pred.will_redirect) {
        wrong_path_pc_ = inst.pc + 4;
      } else if (!real_taken && pred.will_redirect) {
        wrong_path_pc_ = pred.pred_target + 4;
      } else {
        wrong_path_pc_ = pred.pred_target + 4;
      }
      DPRINTF(Pipeline,
              " IF BrPred Wrong -> Dynamic WP from PC=0x%08x",
              wrong_path_pc_);
      last_mispred_tick_ = curr_tick();
    }
  }

  send_ifu_req(candidate->trace_inst.pc);
  fetch_inst_queue_.emplace_back(std::move(candidate));
}

void
Pipeline::do_fetch_1() {
  assert(fetch_inst_queue_.size() <= ifq_size_);
  if (fetch_inst_queue_.size()) {
    assert(sim_pipe_.at(Fetch) == nullptr);
    auto& ptr = fetch_inst_queue_.front();
    if (ptr->wait_mem) {
      schedule(Fetch, InfTime);
      return;
    }
    // RTL: instEmpty = (toidPtr === tailPtr), tailPtr = RegNext.
    // Pop is only possible 1 cycle after R fire.
    if (ptr->ready_tick > curr_tick()) {
      schedule(Fetch, ptr->ready_tick);
      return;
    }
    if (!ptr->is_penalty_fetch)
      sim_pipe_.at(Fetch) = std::move(ptr);
    fetch_inst_queue_.pop_front();
    pop_pending_ = true; // model RTL toidPtr register delay
  }
}

void
Pipeline::send_ifu_req(addr_t addr) {
  [[maybe_unused]] auto const [rready, wready] = imem->is_ready();
  assert(rready);
  imem->read_req(addr);
}

void
Pipeline::handle_ifu_resp() {
  auto it = std::ranges::find_if(
    fetch_inst_queue_, [](const auto& item) { return item->wait_mem; });
  assert(it != fetch_inst_queue_.end());
  auto& ptr = *it;
  ptr->wait_mem = false;
  // RTL: tailPtr = RegNext → pop gated 1 cycle after R fire.
  ptr->ready_tick = curr_tick() + 1;
  DPRINTF(Pipeline, " IF Resp -> PC %08x Penalty %d Ready T@ %lu",
          ptr->trace_inst.pc, ptr->is_penalty_fetch, ptr->ready_tick);
  async_schedule(Fetch, ptr->ready_tick);
}

void
Pipeline::do_decode() {
  const auto& trans = sim_pipe_.at(Fetch);
  const auto& inst = trans->trace_inst;

  // When a memory op is in progress at Execute, maintain LsuStall
  // attribution (RTL: oldest blocked instr determines BlockedCause)
  bool lsu_active = sim_pipe_.at(Execute)
                    && sim_pipe_.at(Execute)->wait_mem;

  auto ready_time =
    std::max(reg_ready_.at(inst.src_reg[0]), reg_ready_.at(inst.src_reg[1]));
  auto rd = inst.dst_reg;
  if (ready_time == InfTime) {
    DPRINTF(Pipeline, " ID -> Blocked : PC=0x%08x src[%d,%d] dst=%d",
            inst.pc, inst.src_reg[0], inst.src_reg[1], inst.dst_reg);
    if (!lsu_active) set_stall(RAW);
    schedule(Decode, InfTime);
    return;
  } else if (ready_time > curr_tick()) {
    if (!lsu_active) set_stall(RAW);
    DPRINTF(Pipeline, " ID -> Until T@ %lu: PC=0x%08x src[%d,%d] dst=%d",
            ready_time, inst.pc, inst.src_reg[0], inst.src_reg[1],
            inst.dst_reg);
    schedule(Decode, ready_time);
    return;
  }
  // Known time
  auto finish_time = std::max(curr_tick(), ready_time) + 1;
  if (!lsu_active) set_stall(NoInst);
  schedule(Decode, finish_time);
  if (rd) {
    reg_ready_.at(rd) = InfTime;
  }
  DPRINTF(Pipeline, " ID -> Pass PC=0x%08x src[%d,%d] dst=%d T@ %lu -> %lu",
          inst.pc, inst.src_reg[0], inst.src_reg[1], inst.dst_reg,
          curr_tick(), finish_time);
  sim_pipe_.at(Decode) = std::move(sim_pipe_.at(Fetch));
}

void
Pipeline::do_execute() {
  assert(stage_update_.at(Decode) <= curr_tick());
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

  DPRINTF(Pipeline, " EX -> PC=0x%08x", inst.pc);

  // Branch misprediction resolved at EX: defer flush by 1 cycle
  // to model RTL BusConnect Pipeline register delay.
  if (trans->br_mispred) {
    pending_flush_ = true;
  }

  if (inst.mem_op == MemNone) {
    if (inst.is_branch && inst.dst_reg != 0) {
      // Jal/Jalr: wbSel!=fromAlu → LS gprFw=false, WB gprFw=true
      // Forwarding available at WB, 2 cycles after EX
      update_reg_time(inst.dst_reg, curr_tick() + 2);
    } else {
      // ALU: wbSel=fromAlu → LS gprFw=true
      // Forwarding available at LS, 1 cycle after EX
      update_reg_time(inst.dst_reg, curr_tick() + 1);
    }
  }

  sim_pipe_.at(Execute) = std::move(sim_pipe_.at(Decode));
  schedule(Execute, curr_tick() + 1);
}

void
Pipeline::do_memory() {
  const auto& trans = sim_pipe_.at(Execute);
  const auto& inst = trans->trace_inst;

  // Don't re-send if already waiting for memory
  if (trans->wait_mem) {
    schedule(Memory, InfTime);
    return;
  }

  if (inst.mem_op == MemLoad) {
    DPRINTF(Pipeline, "LS -> Req [Load] PC=0x%08x addr=0x%08x", inst.pc,
            inst.mem_addr);
    set_stall(LsuStall);
    send_lsu_req(inst.mem_addr, 0xbadU, 0xf, false);
    return;
  } else if (inst.mem_op == MemStore) {
    DPRINTF(Pipeline, "LS -> Req [Store] PC=0x%08x addr=0x%08x", inst.pc,
            inst.mem_addr);
    set_stall(LsuStall);
    send_lsu_req(inst.mem_addr, 0xbadU, 0xf, true);
    schedule(Memory, InfTime);
    return;
  } else {
    // No memory op
    schedule(Memory, curr_tick() + 1);
    sim_pipe_.at(Memory) = std::move(sim_pipe_.at(Execute));
  }
}

void
Pipeline::send_lsu_req(addr_t addr, word_t data, uint8_t strb,
                       bool is_write) {
  auto const [rready, wready] = dmem->is_ready();
  schedule(Memory, InfTime);
  if (is_write ? wready : rready) {
    sim_pipe_.at(Execute)->wait_mem = true;
    auto const aligned = addr & ~0x3U;
    if (is_write) {
      dmem->write_req(aligned, data, strb);
    } else {
      dmem->read_req(aligned);
    }
  }
}

void
Pipeline::handle_lsu_resp() {
  const auto& trans = sim_pipe_.at(Execute);
  assert(trans.get());
  assert(trans->wait_mem);
  assert(!sim_pipe_.at(Memory).get());
  trans->wait_mem = false;
  const auto& inst = trans->trace_inst;

  DPRINTF(Pipeline, " LS -> [%s] Mem Resp : PC=0x%08x addr=0x%08x",
          inst.mem_op == MemLoad ? "Load" : "Store", inst.pc, inst.mem_addr);

  set_stall(NoInst); // LsuStall resolved
  // Apply deferred branch penalty (RTL: branch reaches EX after load clears)
  if (deferred_br_penalty_ > 0) {
    penalty_stall_until_ = std::max(
      penalty_stall_until_, curr_tick() + deferred_br_penalty_);
    deferred_br_penalty_ = 0;
  }
  if (inst.mem_op != MemNone) {
    update_reg_time(inst.dst_reg, curr_tick() + 1);
  }
  // After load completes, check if decode is blocked by RAW dependency.
  // RTL attributes the cycle between load completion and forwarding as RAW
  // (decode sees waitRAW=true since gprFw=false at LS stage).
  if (auto* ids = sim_pipe_.at(Fetch).get()) {
    const auto& idi = ids->trace_inst;
    auto rdy = std::max(reg_ready_.at(idi.src_reg[0]),
                        reg_ready_.at(idi.src_reg[1]));
    if (rdy > curr_tick()) {
      set_stall(RAW);
    }
  }
  async_schedule(Memory, curr_tick() + 1);
  sim_pipe_.at(Memory) = std::move(sim_pipe_.at(Execute));
}

void
Pipeline::do_writeback() {
  ongoing_insts_--;
  DPRINTF(Pipeline, " WB -> PC=0x%08x Remain %lu",
          sim_pipe_.at(Memory)->trace_inst.pc, ongoing_insts_);
  // Flush stall cycles accumulated since last attribution
  flush_stall_cycles(curr_tick());
  stats.nostall++;
  last_attr_tick_ = curr_tick() + 1;
  // Determine stall cause for next cycle
  if (sim_pipe_.at(Execute) && sim_pipe_.at(Execute)->wait_mem) {
    stall_cause_ = LsuStall;
  } else {
    stall_cause_ = NoInst;
  }
  stats.insts++;
  stats.cycles = curr_tick() - reset_tick_;
  sim_pipe_.at(Memory) = nullptr;
  schedule(WriteBack, curr_tick() + 1);
}

void
Pipeline::recv_mem_resp(CpuTrans trans) {
  auto id = trans.id;
  [[maybe_unused]] auto addr = trans.addr;
  [[maybe_unused]] auto is_write = trans.mop == MemRWOpt::Write;
  DPRINTF(Pipeline, "Recv Mem [%s] Resp, ID = %d @ addr %08x",
          is_write ? "Write" : "Read ", id, addr);
  if (id == 0) {
    assert(fetch_inst_queue_.front() != nullptr);
    assert(!is_write);
    handle_ifu_resp();
    return;
  } else if (id == 1) {
    [[maybe_unused]]
    const auto& op = sim_pipe_.at(Execute)->trace_inst.mem_op;
    assert(!is_write && op == MemLoad || is_write && op == MemStore);
    handle_lsu_resp();
  } else {
    assert(false && "No such ID");
  }
}

void
Pipeline::ack_mem_avail(AckTrans ack) {
  auto id = ack.id;
  if (id == 0) {
    // iCache ready: wake pipeline for do_fetch_0 and do_fetch_1.
    async_schedule(Fetch, curr_tick());
    return;
  } else if (id == 1) {
    async_schedule(Memory, curr_tick());
    return;
  } else {
    assert(false && "No such ID");
  }
}

void
Pipeline::update_reg_time(uint8_t rd, tick_t when) {
  if (rd) {
    // Stall 1 cycle after finish
    // ID |stall| --> |
    // EX | --> | ^   ^
    //    forward |   | IDU finished
    reg_ready_.at(rd) = when;
  }
  // Trigger retry of Decode Stage
  if (auto* ids = sim_pipe_.at(Fetch).get()) {
    if (stage_update_.at(Decode) == InfTime) {
      const auto& idi = ids->trace_inst;
      if (idi.src_reg[0] || idi.src_reg[1]) {
        auto ready_time = std::max(reg_ready_.at(idi.src_reg[0]),
                                   reg_ready_.at(idi.src_reg[1]));
        schedule(Decode, ready_time);
      }
    }
  }
  assert(reg_ready_.at(0) == 0);
}

void
Pipeline::flush_wrong_path() {
  // Remove WP entries from fetch queue.  Keep entries that are still
  // waiting for iCache (wait_mem=true) — the iCache will respond
  // naturally and do_fetch_1 will discard them.  Remove ready WP
  // entries (wait_mem=false) immediately.
  auto it = fetch_inst_queue_.begin();
  while (it != fetch_inst_queue_.end()) {
    if ((*it)->is_penalty_fetch && !(*it)->wait_mem) {
      it = fetch_inst_queue_.erase(it);
    } else {
      ++it;
    }
  }
  // Clear Fetch stage if it holds a WP instruction
  if (sim_pipe_.at(Fetch)
      && sim_pipe_.at(Fetch)->is_penalty_fetch) {
    sim_pipe_.at(Fetch) = nullptr;
  }
  // Count WP (wp_this_mispred_ already counts each WP AR from do_fetch_0)
  if (wp_this_mispred_ < 10) wp_hist_[wp_this_mispred_]++;
  else wp_hist_[9]++;
  wp_this_mispred_ = 0;
  DPRINTF(Pipeline, " EX Flush WP: remaining queue %lu",
          fetch_inst_queue_.size());
}

} // namespace pipeSim
