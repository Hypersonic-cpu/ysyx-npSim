#include "pipeSim/Pipeline.hh"
#include "defines/debug.hh"
#include "defines/interface.hh"
#include "defines/types.hh"
#include "trace.hh"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <functional>
#include <ios>
#include <iostream>
#include <memory>
#include <ostream>
#include <utility>

namespace pipeSim {
using trace::MemLoad;
using trace::MemNone;
using trace::MemStore;

Pipeline::Pipeline(const std::string& name, size_t ifq_size, size_t stq_size,
                   BranchUnit* bpu, tick_t br_mis_pen, tick_t mmio_lat)
    : Processor(name, &this->stats, bpu)
    , stats(name)
    , reg_ready_{}
    , stage_update_{}
    , sim_pipe_{}
    , stage_handler_{&Pipeline::do_fetch_1,  &Pipeline::do_decode,
                     &Pipeline::do_execute,  &Pipeline::do_mul_ext,
                     &Pipeline::do_div_ext,  &Pipeline::do_memory,
                     &Pipeline::do_writeback}
    , ifq_size_{ifq_size}
    , fetch_queue_{}
    , ongoing_insts_{0}
    , BranchMissPenalty{br_mis_pen}
    , mmio_lat_{mmio_lat}
    , lsu_serving{Num_PipeStage} {
  assert(bpu && "BranchUnit must not be null");
}

// Main update: process stages back-to-front, then try fetch.
void
Pipeline::update_impl() {
#ifndef NDEBUG
  if (debug::enabled_flags & debug::Event) {
    DPRINTFI(Event, "Regs: ");
    for (int i = 0; i < Num_PipeStage; i++) {
      std::cerr << StageName.at(i) << " [" << std::hex
                << sim_pipe_.at(i).get() << " T" << std::dec
                << stage_update_.at(i) << "], ";
    }
    std::cerr << std::endl;
  }
#endif
  DPRINTF(Event, "Update Pipeline:");
  (void)mmio_resp_tick_;
  // Process stages in reverse topological order.
  // Execute and M-extension are dispatched in parallel from Decode.
  for (int i = Num_PipeStage - 1; i >= 0; i--) {
    if (sim_pipe_.at(i) == nullptr && prev_stage_valid(PipeStage(i))
        && stage_update_.at(i) <= curr_tick()) {
      DPRINTF(Event, "  Invoke stage %s T@ %lu", StageName.at(i).c_str(),
              curr_tick());
      std::invoke(stage_handler_.at(i), this);
    }
  }
  do_fetch_0();
  calc_sched();
}

// Fetch issue: send one request to iCache per cycle.
//
// Branch misprediction model (matches RTL FetchStage):
//
//  T+0: Branch enters IF.  We know it is mispredicted (from trace).
//       The branch itself is issued to iCache normally.
//       Set fetch_.wrong_path = true, fetch_.wrong_path_pc = pc+4.
//
//  T+1: IFU issues wrong-path fetch (pollutes iCache, matches RTL).
//
//  T+2: Branch reaches EX.  do_execute() triggers flush:
//       - Flush all IFQ entries
//       - Clear wrong-path state
//       - Set fetch_.resume_tick = T+2 + BranchMissPenalty
//
//  T+2+penalty: First correct-path fetch issues to iCache.
//
void
Pipeline::do_fetch_0() {
  if (imem->is_ready().first && fetch_queue_.size() < ifq_size_) {
    TransPtr candidate = nullptr;

    if (is_draining_) [[unlikely]] {
      Inst drain_inst{0, 0, 0, false, false, 0, {0, 0}, 0, 0};
      candidate = std::make_unique<Transaction>(drain_inst, true, true);
    } else if (fetch_.wrong_path) {
      // IFU keeps fetching wrong-path PCs until EX flushes
      Inst wp_inst{fetch_.wrong_path_pc, 0, 0, false, false, 0, {0, 0}, 0, 0};
      candidate = std::make_unique<Transaction>(wp_inst, true, true);
      fetch_.wrong_path_pc += 4;
      DPRINTF(Pipeline, " IF WrongPath PC=0x%08x", wp_inst.pc);
    } else if (curr_tick() < fetch_.resume_tick) {
      // Post-flush recovery: cannot fetch until redirect completes
      return;
    } else if (input_buffer_ != nullptr) {
      // Normal fetch of real instruction
      candidate = std::move(input_buffer_);
      candidate->wait_mem = true;

      // Branch prediction at IF stage
      const auto& inst = candidate->trace_inst;
      auto pred = bpu->predict(inst.pc);
      auto real_taken = inst.is_branch && inst.br_taken;
      addr_t real_target = real_taken ? inst.mem_addr : 0;
      auto accurate = bpu->judge(real_taken, real_target, pred);
      candidate->br_pred = pred;
      candidate->br_mispred = !accurate;
      if (inst.is_branch)
        bpu->stats.br_accesses++;
      if (!accurate && !inst.is_branch)
        bpu->stats.nonbr_mispred++;

      if (!accurate) {
        // Misprediction detected at IF.  Enter wrong-path mode.
        fetch_.wrong_path = true;
        flush_stall_cycles(curr_tick());
        stall_.in_br_recovery = true;

        if (real_taken && !pred.will_redirect) {
          // Predicted not-taken but should take: IFU fetches pc+4
          fetch_.wrong_path_pc = inst.pc + 4;
        } else if (!real_taken && pred.will_redirect) {
          // Predicted taken but should not: IFU fetches pred target
          fetch_.wrong_path_pc = pred.pred_target;
        } else {
          // Wrong target
          fetch_.wrong_path_pc = pred.pred_target;
        }

        DPRINTF(Pipeline, " IF BrMispred PC=0x%08x -> WrongPath @0x%08x",
                inst.pc, fetch_.wrong_path_pc);
      }
    } else {
      return;
    }

    [[maybe_unused]] auto const [rdy, _] = imem->is_ready();
    assert(rdy);
    // SoC mode: wrong-path fetches allocate in iCache and pollute,
    // matching RTL behavior (iCache does not know about wrong-path).
    // NPC mode: speculative (no fill), calibrated with fixed latency.
    if (!g_soc_mode && candidate->is_wrong_path)
      imem->read_req_speculative(candidate->trace_inst.pc);
    else
      imem->read_req(candidate->trace_inst.pc);
    fetch_queue_.emplace_back(std::move(candidate));
  }
}

// Fetch stage 1: IFQ head to pipeline[Fetch]
void
Pipeline::do_fetch_1() {
  assert(fetch_queue_.size() <= ifq_size_);
  if (fetch_queue_.empty()) {
    schedule(Fetch, InfTime);
    return;
  }
  auto& ptr = fetch_queue_.front();
  if (ptr->wait_mem) {
    schedule(Fetch, InfTime);
    return;
  }
  if (ptr->is_wrong_path) {
    if (g_soc_mode) {
      // SoC: IDU drains wrong-path entries at 1/cycle,
      // freeing IFQ slots for new wrong-path fetches.
      fetch_queue_.pop_front();
      schedule(Fetch, curr_tick() + 1);
    } else {
      // NPC: hold wrong-path entries until EX flush.
      schedule(Fetch, InfTime);
    }
    return;
  }
  assert(sim_pipe_.at(Fetch) == nullptr);
  sim_pipe_.at(Fetch) = std::move(ptr);
  fetch_queue_.pop_front();
}

// iCache response handler
void
Pipeline::handle_ifu_resp() {
  // Orphan response: iCache request was in-flight when EX flushed
  if (fetch_.orphan_resps > 0) {
    fetch_.orphan_resps--;
    DPRINTF(Pipeline, " IF Resp -> Orphan (ignored), %lu remain",
            fetch_.orphan_resps);
    return;
  }
  auto it = std::ranges::find_if(
    fetch_queue_, [](const auto& item) { return item->wait_mem; });
  assert(it != fetch_queue_.end());
  (*it)->wait_mem = false;
  DPRINTF(Pipeline, " IF Resp -> PC %08x WP=%d T@ %lu", (*it)->trace_inst.pc,
          (*it)->is_wrong_path, curr_tick() + 1);
  async_schedule(Fetch, curr_tick() + 1);
}

// Decode
void
Pipeline::do_decode() {
  const auto& trans = sim_pipe_.at(Fetch);
  const auto& inst = trans->trace_inst;

  bool lsu_active = sim_pipe_.at(Execute) && sim_pipe_.at(Execute)->wait_mem;

  // M-extension scoreboard: block ALL instructions while MUL or DIV
  // is in-flight.  Matches RTL Dispatcher which stalls IDU whenever
  // any scoreboard bit is set (scoreboard.orR).
  if (sim_pipe_.at(IntMulExt) || sim_pipe_.at(IntDivExt)) {
    if (!lsu_active)
      set_stall(RAW);
    tick_t ready = InfTime;
    if (sim_pipe_.at(IntMulExt))
      ready = std::min(ready, mul_ready_tick_);
    if (sim_pipe_.at(IntDivExt))
      ready = std::min(ready, div_ready_tick_);
    schedule(Decode, ready);
    DPRINTF(Pipeline, " ID M-ext scoreboard stall PC=0x%08x until T@%lu",
            inst.pc, ready);
    return;
  }

  // RAW hazard check
  auto ready_time =
    std::max(reg_ready_.at(inst.src_reg[0]), reg_ready_.at(inst.src_reg[1]));
  if (ready_time == InfTime) {
    DPRINTF(Pipeline, " ID Blocked: PC=0x%08x src[%d,%d]", inst.pc,
            inst.src_reg[0], inst.src_reg[1]);
    if (!lsu_active)
      set_stall(RAW);
    schedule(Decode, InfTime);
    return;
  } else if (ready_time > curr_tick()) {
    if (!lsu_active)
      set_stall(RAW);
    DPRINTF(Pipeline, " ID Stall until T@%lu: PC=0x%08x", ready_time,
            inst.pc);
    schedule(Decode, ready_time);
    return;
  }

  // WAW hazard: stall if an in-flight M-ext instruction writes the
  // same dst_reg.  The EXU->MEM path is faster than Mul/Div, so
  // without this stall the later (EXU) instruction would WB first
  // and the Mul/Div would incorrectly overwrite its result.
  if (inst.dst_reg != 0) {
    auto has_mext_waw = [&](PipeStage s) {
      auto& sp = sim_pipe_.at(s);
      return sp && sp->trace_inst.dst_reg == inst.dst_reg;
    };
    if (has_mext_waw(IntMulExt) || has_mext_waw(IntDivExt)) {
      if (!lsu_active)
        set_stall(RAW);
      schedule(Decode, reg_ready_.at(inst.dst_reg));
      DPRINTF(Pipeline, " ID WAW stall PC=0x%08x dst=%d until T@%lu",
              inst.pc, inst.dst_reg, reg_ready_.at(inst.dst_reg));
      return;
    }
  }

  auto finish_time = std::max(curr_tick(), ready_time) + 1;
  if (!lsu_active)
    set_stall(NoInst);
  schedule(Decode, finish_time);
  if (inst.dst_reg)
    reg_ready_.at(inst.dst_reg) = InfTime;

  DPRINTF(Pipeline, " ID Pass PC=0x%08x src[%d,%d] dst=%d T@%lu->%lu",
          inst.pc, inst.src_reg[0], inst.src_reg[1], inst.dst_reg,
          curr_tick(), finish_time);
  sim_pipe_.at(Decode) = std::move(sim_pipe_.at(Fetch));
}

// Execute
void
Pipeline::do_execute() {
  assert(stage_update_.at(Decode) <= curr_tick());
  const auto& trans = sim_pipe_.at(Decode);
  const auto& inst = trans->trace_inst;
  // Skip M-extension instructions (routed by do_mul_ext/do_div_ext)
  if (inst.ext_op != trace::ExtNone)
    return;

  // Branch resolution at EX -- matches RTL flushWire
  if (inst.is_branch) {
    bool real_taken = inst.br_taken != 0;
    addr_t real_target = real_taken ? inst.mem_addr : 0;
    bool is_call = (inst.dst_reg == 1);
    bool is_ret = (inst.src_reg[0] == 1 && inst.dst_reg == 0);
    bool btb_hit = (trans->br_pred.pred_target != 0);
    bpu->update(inst.pc, real_taken, real_target, is_call, is_ret, btb_hit);

    if (trans->br_mispred) {
      DPRINTF(Pipeline, " EX Flush PC=0x%08x taken=%d target=0x%08x",
              inst.pc, real_taken, real_target);

      // Flush fetch queue: wrong-path discarded, real entries get
      // ongoing_insts_ decremented.  Track orphaned iCache requests.
      for (auto& fq_entry : fetch_queue_) {
        if (fq_entry) {
          if (fq_entry->wait_mem)
            fetch_.orphan_resps++;
          if (!fq_entry->is_wrong_path)
            ongoing_insts_--;
        }
      }
      fetch_queue_.clear();
      fetch_.wrong_path = false;
      // RTL iCache pipe is NOT flushed on branch misprediction:
      // wrong-path fills complete and pollute the cache.

      fetch_.resume_tick = curr_tick() + BranchMissPenalty;
      flush_stall_cycles(curr_tick());
      stall_.in_br_recovery = false;
    }
  }

  // Non-branch misprediction: BTB aliasing predicts a non-branch
  // as taken.  RTL EXU flushes in this case.
  flush_false_btb_hit(*trans);

  // Register forwarding -- matches RTL:
  // EXU: gprFw=false (no forwarding from EX)
  // LSU: gprFw=(wbSel==fromAlu) -- ALU results forward at LS
  // WBU: always forwards
  if (inst.mem_op == MemNone) {
    if (inst.is_branch && inst.dst_reg != 0) {
      // Jal/Jalr: wbSel!=fromAlu, no LS forward, WB forwards
      update_reg_time(inst.dst_reg, curr_tick() + 2);
    } else {
      // ALU: wbSel=fromAlu, LS forwards (1 cycle after EX)
      update_reg_time(inst.dst_reg, curr_tick() + 1);
    }
  }

  sim_pipe_.at(Execute) = std::move(sim_pipe_.at(Decode));
  schedule(Execute, curr_tick() + 1);
}

void
Pipeline::do_mul_ext() {
  assert(stage_update_.at(Decode) <= curr_tick());
  const auto& trans = sim_pipe_.at(Decode);
  const auto& inst = trans->trace_inst;
  if (!(inst.ext_op == trace::IntMulH || inst.ext_op == trace::IntMulL))
    return;

  flush_false_btb_hit(*trans);

  constexpr tick_t MulLat = 2;
  mul_ready_tick_ = curr_tick() + MulLat;
  // RTL: WBU always forwards; result available when scoreboard clears
  update_reg_time(inst.dst_reg, curr_tick() + MulLat);
  schedule(IntMulExt, curr_tick() + MulLat);
  async_schedule(Memory, curr_tick() + MulLat);
  sim_pipe_.at(IntMulExt) = std::move(sim_pipe_.at(Decode));
}

void
Pipeline::do_div_ext() {
  assert(stage_update_.at(Decode) <= curr_tick());
  const auto& trans = sim_pipe_.at(Decode);
  const auto& inst = trans->trace_inst;
  if (!(inst.ext_op == trace::IntDiv || inst.ext_op == trace::IntRem))
    return;

  flush_false_btb_hit(*trans);

  constexpr tick_t DivLat = 33;
  div_ready_tick_ = curr_tick() + DivLat;
  // RTL: WBU always forwards; result available when scoreboard clears
  update_reg_time(inst.dst_reg, curr_tick() + DivLat);
  schedule(IntDivExt, curr_tick() + DivLat);
  async_schedule(Memory, curr_tick() + DivLat);
  sim_pipe_.at(IntDivExt) = std::move(sim_pipe_.at(Decode));
}

// Memory
void
Pipeline::do_memory() {
  if (this->lsu_serving == Num_PipeStage) {
    // Priority: Div (if ready) > Mul (if ready) > EXU
    if (sim_pipe_.at(IntDivExt) && curr_tick() >= div_ready_tick_) {
      this->lsu_serving = IntDivExt;
    } else if (sim_pipe_.at(IntMulExt) && curr_tick() >= mul_ready_tick_) {
      this->lsu_serving = IntMulExt;
    } else if (sim_pipe_.at(Execute)) {
      this->lsu_serving = Execute;
    } else {
      // M-ext in-flight but not yet ready
      tick_t next = InfTime;
      if (sim_pipe_.at(IntDivExt))
        next = std::min(next, div_ready_tick_);
      if (sim_pipe_.at(IntMulExt))
        next = std::min(next, mul_ready_tick_);
      if (next < InfTime)
        schedule(Memory, next);
      return;
    }
  }
  auto& trans = sim_pipe_.at(this->lsu_serving);
  assert(trans && "LSU got empty transaction");
  const auto& inst = trans->trace_inst;
  assert(!inst.ext_op || !inst.mem_op && "M-extension insts mem access");

  // SoC MMIO: second pass after latency elapsed
  if (trans->wait_mem && g_soc_mode && inst.mem_op != MemNone) {
    uint8_t top = (inst.mem_addr >> 28) & 0xf;
    bool cacheable =
      (top == 0x3 || top == 0x8 || top == 0x9 || top == 0xa || top == 0xb);
    if (!cacheable) {
      trans->wait_mem = false;
      if (inst.mem_op == MemLoad)
        update_reg_time(inst.dst_reg, curr_tick() + 1);
      schedule(Memory, curr_tick() + 1);
      sim_pipe_.at(Memory) = std::move(trans);
      this->lsu_serving = Num_PipeStage;
      return;
    }
  }

  if (trans->wait_mem) {
    schedule(Memory, InfTime);
    return;
  }

  // SoC MMIO: first pass -- block pipeline for mmio_lat_ cycles
  if (g_soc_mode && inst.mem_op != MemNone) {
    uint8_t top = (inst.mem_addr >> 28) & 0xf;
    bool cacheable =
      (top == 0x3 || top == 0x8 || top == 0x9 || top == 0xa || top == 0xb);
    if (!cacheable) {
      set_stall(LsuStall);
      trans->wait_mem = true;
      schedule(Memory, curr_tick() + mmio_lat_);
      return;
    }
  }

  if (inst.mem_op == MemLoad) {
    DPRINTF(Pipeline, " LS Load PC=0x%08x addr=0x%08x", inst.pc,
            inst.mem_addr);
    set_stall(LsuStall);
    send_lsu_req(inst.mem_addr, 0xbadU, 0xf, false);
    return;
  } else if (inst.mem_op == MemStore) {
    DPRINTF(Pipeline, " LS Store PC=0x%08x addr=0x%08x", inst.pc,
            inst.mem_addr);
    set_stall(LsuStall);
    send_lsu_req(inst.mem_addr, 0xbadU, 0xf, true);
    schedule(Memory, InfTime);
    return;
  }
  // No memory op -- pass through
  schedule(Memory, curr_tick() + 1);
  sim_pipe_.at(Memory) = std::move(sim_pipe_.at(this->lsu_serving));
  this->lsu_serving = Num_PipeStage;
}

void
Pipeline::send_lsu_req(addr_t addr, word_t data, uint8_t strb,
                       bool is_write) {
  auto const [rready, wready] = dmem->is_ready();
  schedule(Memory, InfTime);
  if (is_write ? wready : rready) {
    sim_pipe_.at(Execute)->wait_mem = true;
    auto const aligned = addr & ~0x3U;
    if (is_write)
      dmem->write_req(aligned, data, strb);
    else
      dmem->read_req(aligned);
  }
}

void
Pipeline::handle_lsu_resp() {
  const auto& trans = sim_pipe_.at(Execute);
  assert(trans.get() && trans->wait_mem);
  assert(!sim_pipe_.at(Memory).get());
  trans->wait_mem = false;
  const auto& inst = trans->trace_inst;

  DPRINTF(Pipeline, " LS Resp [%s] PC=0x%08x addr=0x%08x",
          inst.mem_op == MemLoad ? "Load" : "Store", inst.pc, inst.mem_addr);

  this->lsu_serving = Num_PipeStage;
  set_stall(NoInst);
  if (inst.mem_op != MemNone)
    update_reg_time(inst.dst_reg, curr_tick() + 1);

  // Check if decode is blocked by RAW from this load
  if (auto* ids = sim_pipe_.at(Fetch).get()) {
    const auto& idi = ids->trace_inst;
    auto rdy =
      std::max(reg_ready_.at(idi.src_reg[0]), reg_ready_.at(idi.src_reg[1]));
    if (rdy > curr_tick())
      set_stall(RAW);
  }
  async_schedule(Memory, curr_tick() + 1);
  sim_pipe_.at(Memory) = std::move(sim_pipe_.at(Execute));
}

// WriteBack
void
Pipeline::do_writeback() {
  ongoing_insts_--;
  DPRINTF(Pipeline, " WB PC=0x%08x Remain %lu",
          sim_pipe_.at(Memory)->trace_inst.pc, ongoing_insts_);
  flush_stall_cycles(curr_tick());
  stats.nostall++;
  stall_.last_tick = curr_tick() + 1;

  auto* trans = sim_pipe_.at(Memory).get();
  if (trans->br_mispred) {
    stall_.cause = BrMispred;
    stall_.brmiss_attr_end = curr_tick() + 3;
  } else if (sim_pipe_.at(Execute) && sim_pipe_.at(Execute)->wait_mem) {
    stall_.cause = LsuStall;
  } else {
    stall_.cause = NoInst;
  }

  stats.insts++;
  stats.cycles = curr_tick() - stall_.reset_tick;
  sim_pipe_.at(Memory) = nullptr;
  schedule(WriteBack, curr_tick() + 1);
}

// Mem response dispatch
void
Pipeline::recv_mem_resp(CpuTrans trans) {
  auto id = trans.id;
  [[maybe_unused]] auto addr = trans.addr;
  [[maybe_unused]] auto is_write = trans.mop == MemRWOpt::Write;
  DPRINTF(Pipeline, "Recv [%s] Resp, ID=%d addr=%08x",
          is_write ? "Write" : "Read", id, addr);
  if (id == 0) {
    assert(!is_write);
    handle_ifu_resp();
  } else if (id == 1) {
    handle_lsu_resp();
  } else {
    assert(false && "No such ID");
  }
}

void
Pipeline::ack_mem_avail(AckTrans ack) {
  auto id = ack.id;
  if (id == 0)
    async_schedule(Fetch, curr_tick());
  else if (id == 1) {
    auto& ex = sim_pipe_.at(Execute);
    if (ex && !ex->wait_mem && ex->trace_inst.mem_op != MemNone)
      async_schedule(Memory, curr_tick());
  } else
    assert(false && "No such ID");
}

// Register ready time update
void
Pipeline::update_reg_time(uint8_t rd, tick_t when) {
  if (rd)
    reg_ready_.at(rd) = when;
  // Retry decode if it was blocked waiting on this register
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

// False BTB hit: BPU predicted a non-branch as taken.
// RTL EXU flushes in this case.  Called from EX and M-ext stages.
void
Pipeline::flush_false_btb_hit(const Transaction& trans) {
  const auto& inst = trans.trace_inst;
  if (inst.is_branch || !trans.br_mispred)
    return;
  DPRINTF(Pipeline, " EX NonBr Flush PC=0x%08x (false BTB hit)", inst.pc);
  for (auto& fq_entry : fetch_queue_) {
    if (fq_entry) {
      if (fq_entry->wait_mem)
        fetch_.orphan_resps++;
      if (!fq_entry->is_wrong_path)
        ongoing_insts_--;
    }
  }
  fetch_queue_.clear();
  fetch_.wrong_path = false;
  fetch_.resume_tick = curr_tick() + BranchMissPenalty;
  flush_stall_cycles(curr_tick());
  stall_.in_br_recovery = false;
}

} // namespace pipeSim
