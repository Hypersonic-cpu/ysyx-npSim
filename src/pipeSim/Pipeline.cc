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
    , fetch_queue_(ifq_size)
    , ongoing_insts_{0}
    , BranchMissPenalty{br_mis_pen}
    , mmio_lat_{mmio_lat} {
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
  // Reset per-tick BPU SRAM state before processing any stage.
  bpu->begin_tick();
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
  if (!imem->is_ready().first)
    return;

  // earlyRedirect: after a correctly predicted taken branch, the
  // IFU continues fetching sequential PCs for 2 cycles before the
  // BPU redirect takes effect.  Matches RTL FetchStage where
  // earlyRedirect fires 2 cycles after receipt (iCache 1cyc +
  // BPU SyncReadMem 1cyc).  These fetches pollute iCache but
  // their responses are discarded.
  if (fetch_.early_redir_remaining > 0) {
    imem->read_req(fetch_.early_redir_seq_pc);
    fetch_.resp_is_orphan.push_back(true);
    DPRINTF(Pipeline, " IF EarlyRedir PC=0x%08x (%d left)",
            fetch_.early_redir_seq_pc,
            fetch_.early_redir_remaining - 1);
    fetch_.early_redir_seq_pc += 4;
    fetch_.early_redir_remaining--;
    return;
  }

  // Wrong-path: issue iCache request for cache pollution but do NOT
  // push to IFQ.  In RTL, wrong-path instructions flow through the
  // full pipeline (FetchBuf -> Decode -> Dispatch -> Execute),
  // occupying pipeline slots and extending the misprediction window.
  // Modeling them as IFQ entries that drain at 1/cycle is too fast
  // (IFQ fills up, capping wrong-path count).  Instead, treat each
  // wrong-path fetch as fire-and-forget: iCache processes the
  // request (pollution), and the response is consumed as orphan.
  if (fetch_.wrong_path) {
    // Post-flush: stop WP when penalty window ends
    if (fetch_.wp_flushed && curr_tick() >= fetch_.resume_tick) {
      fetch_.wrong_path = false;
      fetch_.wp_flushed = false;
      // Fall through to normal fetch below
    } else {
    // BPU predicts on wrong-path PC (matches RTL SyncReadMem query)
    auto wp_pred = bpu->predict(fetch_.wrong_path_pc);
    stats.wp_bpu_queries++;

    // Issue iCache request for cache pollution
    imem->read_req(fetch_.wrong_path_pc);
    fetch_.resp_is_orphan.push_back(true);

    DPRINTF(Pipeline, " IF WrongPath PC=0x%08x", fetch_.wrong_path_pc);

    // Default next: sequential
    fetch_.wrong_path_pc += 4;

    // 2-cycle redirect pipeline (matches RTL earlyRedirect):
    //  T:   fetch X, BPU query X -> stage1
    //  T+1: fetch X+4, stage1->stage2
    //  T+2: fetch X+8, stage2 fires -> redirect to Y
    bool redirected_this_cycle = false;
    if (fetch_.wp_redirect_s2) {
      fetch_.wrong_path_pc = fetch_.wp_redirect_target_s2;
      fetch_.wp_redirect_s2 = false;
      fetch_.wp_redirect_s1 = false;
      redirected_this_cycle = true;
      stats.wp_redirects++;
    }

    if (fetch_.wp_redirect_s1) {
      fetch_.wp_redirect_s2 = true;
      fetch_.wp_redirect_target_s2 = fetch_.wp_redirect_target_s1;
      fetch_.wp_redirect_s1 = false;
    }

    if (wp_pred.will_redirect && !redirected_this_cycle) {
      fetch_.wp_redirect_s1 = true;
      fetch_.wp_redirect_target_s1 = wp_pred.pred_target;
      stats.wp_btb_hits++;
    }
    return;
    }
  }

  if (fetch_queue_.full())
    return;

  if (is_draining_) [[unlikely]] {
    Inst drain_inst{0, 0, 0, false, false, 0, {0, 0}, 0, 0};
    auto candidate =
      std::make_unique<Transaction>(drain_inst, true, true);
    imem->read_req(candidate->trace_inst.pc);
    fetch_.resp_is_orphan.push_back(false);
    fetch_queue_.push_back(std::move(candidate));
    return;
  }

  if (curr_tick() < fetch_.resume_tick)
    return;

  if (input_buffer_ == nullptr)
    return;

  // Normal fetch of real instruction
  auto candidate = std::move(input_buffer_);
  candidate->wait_mem = true;

  const auto& inst = candidate->trace_inst;
  auto pred = bpu->predict_at_fetch(inst.pc, inst.is_branch);
  auto real_taken = inst.is_branch && inst.br_taken;
  addr_t real_target = real_taken ? inst.mem_addr : 0;
  auto accurate = bpu->judge(real_taken, real_target, pred);
  candidate->br_pred = pred;
  candidate->br_mispred = !accurate;
  if (!accurate && !inst.is_branch)
    bpu->stats.nonbr_mispred++;

  if (!accurate) {
    fetch_.wrong_path = true;
    flush_stall_cycles(curr_tick());
    stall_.in_br_recovery = true;

    if (real_taken && !pred.will_redirect) {
      fetch_.wrong_path_pc = inst.pc + 4;
    } else if (!real_taken && pred.will_redirect) {
      fetch_.wrong_path_pc = pred.pred_target;
    } else {
      fetch_.wrong_path_pc = pred.pred_target;
    }

    DPRINTF(Pipeline, " IF BrMispred PC=0x%08x -> WrongPath @0x%08x",
            inst.pc, fetch_.wrong_path_pc);
  }

  imem->read_req(candidate->trace_inst.pc);
  fetch_.resp_is_orphan.push_back(false);

  // TAGE timing model: TAGE result is 1 cycle later than bimodal.
  // If TAGE overrode the bimodal direction (and BTB hit), the fetch
  // based on bimodal is wrong; stall IFU for 1 cycle for the redirect.
  // This is applied even when the overall prediction is accurate
  // (TAGE cost still exists when it corrects bimodal).
  if (pred.tage_overrode_bimodal) {
    fetch_.resume_tick = std::max(fetch_.resume_tick, curr_tick() + 2);
    DPRINTF(Pipeline, " IF TAGE-override stall PC=0x%08x", inst.pc);
  }

  // earlyRedirect trigger: correctly predicted taken branch.
  // RTL's IFU fetches 2 sequential PCs (PC+4, PC+8) before
  // the BPU redirect takes effect.  These go through iCache
  // (cache pollution) but results are discarded as orphans.
  if (accurate && pred.will_redirect) {
    fetch_.early_redir_remaining = 2;
    fetch_.early_redir_seq_pc = inst.pc + 4;
    stats.early_redirects++;
  }

  fetch_queue_.push_back(std::move(candidate));
}

// Fetch stage 1: IFQ head to pipeline[Fetch]
void
Pipeline::do_fetch_1() {
  if (fetch_queue_.empty()) {
    schedule(Fetch, InfTime);
    return;
  }
  auto& ptr = fetch_queue_.front();
  if (ptr->wait_mem) {
    schedule(Fetch, InfTime);
    return;
  }
  // Wrong-path entries are no longer pushed to IFQ (they bypass it
  // as fire-and-forget iCache requests), so this assert verifies
  // no wrong-path entry leaked into the queue.
  assert(!ptr->is_wrong_path);
  assert(sim_pipe_.at(Fetch) == nullptr);
  sim_pipe_.at(Fetch) = std::move(ptr);
  fetch_queue_.pop_front();
}

// iCache response handler
void
Pipeline::handle_ifu_resp() {
  // Response ordering FIFO: pop front to determine if this response
  // is for a real IFQ entry or an orphan (wrong-path / flushed).
  assert(!fetch_.resp_is_orphan.empty());
  bool is_orphan = fetch_.resp_is_orphan.front();
  fetch_.resp_is_orphan.pop_front();
  if (is_orphan) {
    DPRINTF(Pipeline, " IF Resp -> Orphan (ignored), %zu remain",
            fetch_.resp_is_orphan.size());
    return;
  }
  auto* ptr = fetch_queue_.find_if_ptr(
    [](const TransPtr& item) { return item->wait_mem; });
  assert(ptr != nullptr);
  (*ptr)->wait_mem = false;
  async_schedule(Fetch, curr_tick() + 1);
}

// Decode
void
Pipeline::do_decode() {
  const auto& trans = sim_pipe_.at(Fetch);
  const auto& inst = trans->trace_inst;

  bool lsu_active = sim_pipe_.at(Execute) && sim_pipe_.at(Execute)->wait_mem;

  // M-extension scoreboard: only block next MUL/DIV instruction
  // when any MUL/DIV is in-flight.  ALU instructions proceed
  // freely (RAW deps handled by reg_ready below).
  // Matches RTL Dispatcher: tgtReady = unitReady && Mux(isMD, !sbAnyBusy, true)
  if (inst.ext_op != trace::ExtNone) {
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
    bpu->update(inst.pc, real_taken, real_target, is_call, is_ret, btb_hit,
                trans->br_pred, trans->br_mispred);

    if (trans->br_mispred) {
      DPRINTF(Pipeline, " EX Flush PC=0x%08x taken=%d target=0x%08x",
              inst.pc, real_taken, real_target);

      // Flush fetch queue: only correct-path entries remain in IFQ
      // (wrong-path fetches bypass the IFQ entirely).
      // Mark any pending (wait_mem) IFQ entries as orphans in the
      // response FIFO so their iCache responses are discarded.
      for (auto& is_orph : fetch_.resp_is_orphan) {
        if (!is_orph)
          is_orph = true;
      }
      for (size_t i = 0; i < fetch_queue_.size(); i++) {
        auto& fq_entry = fetch_queue_.at(i);
        if (fq_entry)
          ongoing_insts_--;
      }
      fetch_queue_.clear();
      fetch_.wp_flushed = true;
      fetch_.wp_redirect_s1 = false;
      fetch_.wp_redirect_s2 = false;
      fetch_.early_redir_remaining = 0;
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
  // EXU: gprFw=(wbSel==fromAlu) -- ALU results forward combinationally
  // LSU: gprFw=(wbSel==fromAlu) -- same as EX (for address, not data)
  // WBU: always forwards
  if (inst.mem_op == MemNone) {
    if (inst.is_branch && inst.dst_reg != 0) {
      // Jal/Jalr: wbSel=fromPC, gprFw=false at EX/SKID/LS, WB forwards
      update_reg_time(inst.dst_reg, curr_tick() + 2);
    } else {
      // ALU: wbSel=fromAlu, EX forwards same cycle (combinational)
      update_reg_time(inst.dst_reg, curr_tick());
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

  constexpr tick_t MulLat = 4;
  mul_ready_tick_ = curr_tick() + MulLat;
  // RTL: WBU always forwards; result available when scoreboard clears
  update_reg_time(inst.dst_reg, curr_tick() + MulLat);
  schedule(IntMulExt, curr_tick() + MulLat);
  // MUL bypasses Memory stage and writes back directly.
  async_schedule(WriteBack, curr_tick() + MulLat);
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

  constexpr tick_t DivLat = 34;
  div_ready_tick_ = curr_tick() + DivLat;
  // RTL: WBU always forwards; result available when scoreboard clears
  update_reg_time(inst.dst_reg, curr_tick() + DivLat);
  schedule(IntDivExt, curr_tick() + DivLat);
  // DIV bypasses Memory stage and writes back directly.
  async_schedule(WriteBack, curr_tick() + DivLat);
  sim_pipe_.at(IntDivExt) = std::move(sim_pipe_.at(Decode));
}

// Memory: EXU path only.  MUL/DIV go directly to WriteBack.
void
Pipeline::do_memory() {
  auto& trans = sim_pipe_.at(Execute);
  assert(trans && "Memory stage: no Execute instruction");
  const auto& inst = trans->trace_inst;

  // MMIO: second pass after latency elapsed
  if (trans->wait_mem && inst.mem_op != MemNone) {
    uint8_t top = (inst.mem_addr >> 28) & 0xf;
    bool cacheable =
      (top == 0x3 || top == 0x8 || top == 0x9 || top == 0xa || top == 0xb);
    if (!cacheable) {
      trans->wait_mem = false;
      if (inst.mem_op == MemLoad)
        update_reg_time(inst.dst_reg, curr_tick() + 2);
      schedule(Memory, curr_tick() + 1);
      sim_pipe_.at(Memory) = std::move(sim_pipe_.at(Execute));
      return;
    }
  }

  if (trans->wait_mem) {
    schedule(Memory, InfTime);
    return;
  }

  // MMIO: first pass -- block pipeline for mmio_lat_ cycles
  if (inst.mem_op != MemNone) {
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
  // No memory op -- pass through to Memory slot
  schedule(Memory, curr_tick() + 1);
  sim_pipe_.at(Memory) = std::move(sim_pipe_.at(Execute));
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

  if (inst.mem_op == MemLoad)
    update_reg_time(inst.dst_reg, curr_tick() + 1);

  set_stall(NoInst);

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

// WriteBack: selects from Memory (EXU path), DivExt, or MulExt.
// Priority: Memory > DIV > MUL, matching RTL Collector
// (Collector.canMD = !aluSide.valid && !pendingALU).
void
Pipeline::do_writeback() {
  PipeStage src;
  // pendingALU guard: MUL/DIV can only commit when no ALU instruction
  // is between Decode and Memory (matches RTL aluInFlight counter).
  bool pending_alu = (sim_pipe_.at(Execute) != nullptr);
  if (sim_pipe_.at(Memory))
    src = Memory;
  else if (!pending_alu && sim_pipe_.at(IntDivExt)
           && curr_tick() >= div_ready_tick_)
    src = IntDivExt;
  else if (!pending_alu && sim_pipe_.at(IntMulExt)
           && curr_tick() >= mul_ready_tick_)
    src = IntMulExt;
  else {
    // No source ready yet; schedule for earliest completion
    tick_t next = InfTime;
    if (sim_pipe_.at(IntDivExt))
      next = std::min(next, div_ready_tick_);
    if (sim_pipe_.at(IntMulExt))
      next = std::min(next, mul_ready_tick_);
    schedule(WriteBack, next);
    return;
  }

  ongoing_insts_--;
  DPRINTF(Pipeline, " WB PC=0x%08x Remain %lu",
          sim_pipe_.at(src)->trace_inst.pc, ongoing_insts_);
  flush_stall_cycles(curr_tick());
  stats.nostall++;
  stall_.last_tick = curr_tick() + 1;

  auto* trans = sim_pipe_.at(src).get();
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
  sim_pipe_.at(src) = nullptr;
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
// RTL EXU flushes and clears the BTB entry in this case.
void
Pipeline::flush_false_btb_hit(const Transaction& trans) {
  const auto& inst = trans.trace_inst;
  if (inst.is_branch)
    return;

  // RTL: updValid fires for (isBr || predBtbHit).  For non-branches
  // with a BTB hit, btbUpdWen clears the entry regardless of BHT
  // direction.  In npsim the BTB hit was captured at predict time
  // as pred_target != 0.
  bool had_btb_hit = (trans.br_pred.pred_target != 0);
  if (had_btb_hit && !trans.br_mispred) {
    // BTB false hit but BHT said not-taken -> no redirect, no flush.
    // Still clear the BTB entry (matches RTL).
    DPRINTF(Pipeline, " EX NonBr SilentClear PC=0x%08x (false BTB hit,"
            " no redirect)", inst.pc);
    bpu->clear_btb_entry(inst.pc);
    return;
  }

  if (!trans.br_mispred)
    return;

  DPRINTF(Pipeline, " EX NonBr Flush PC=0x%08x (false BTB hit)", inst.pc);
  // Clear the aliased BTB entry (matches RTL btbUpdWen for !isBranch)
  bpu->clear_btb_entry(inst.pc);
  for (auto& is_orph : fetch_.resp_is_orphan) {
    if (!is_orph)
      is_orph = true;
  }
  for (size_t i = 0; i < fetch_queue_.size(); i++) {
    auto& fq_entry = fetch_queue_.at(i);
    if (fq_entry)
      ongoing_insts_--;
  }
  fetch_queue_.clear();
  fetch_.wp_flushed = true;
  fetch_.wp_redirect_s1 = false;
  fetch_.wp_redirect_s2 = false;
  fetch_.early_redir_remaining = 0;

  fetch_.resume_tick = curr_tick() + BranchMissPenalty;
  flush_stall_cycles(curr_tick());
  stall_.in_br_recovery = false;
}

} // namespace pipeSim
