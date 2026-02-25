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

Pipeline::Pipeline(const std::string& name, size_t ifq_size,
                   size_t stq_size, BranchUnit* bpu,
                   tick_t br_mis_pen, bool soc_mode)
    : Processor(name, &this->stats, bpu)
    , stats(name)
    , reg_ready_{}
    , stage_update_{}
    , sim_pipe_{}
    , stage_handler_{&Pipeline::do_fetch_1, &Pipeline::do_decode,
                     &Pipeline::do_execute, &Pipeline::do_memory,
                     &Pipeline::do_writeback}
    , ifq_size_{ifq_size}
    , fetch_queue_{}
    , ongoing_insts_{0}
    , soc_mode_{soc_mode}
    , BranchMissPenalty{br_mis_pen} {
  assert(bpu && "BranchUnit must not be null");
}

// ── Main update: process stages back-to-front, then try fetch ────
void
Pipeline::update_impl() {
  DPRINTF(Event, "Update Pipeline:");
  for (int i = Num_PipeStage - 1; i >= 0; i--) {
    if (sim_pipe_.at(i) == nullptr
        && (!i || sim_pipe_.at(i - 1) != nullptr)
        && stage_update_.at(i) <= curr_tick()) {
      DPRINTF(Event, "  Move (%s -> %s) T@ %lu",
              StageName.at(std::max(0, i - 1)).c_str(),
              StageName.at(i).c_str(), curr_tick());
      std::invoke(stage_handler_.at(i), this);
    }
  }
  try_issue_fetch();
  calc_sched();
}

// ── Fetch issue: send requests to iCache ─────────────────────────
//
// Branch misprediction model (matches RTL FetchStage):
//
//  T+0: Branch enters IF. We know it's mispredicted (from trace).
//       The branch itself is issued to iCache normally.
//       Set in_wrong_path_ = true, wrong_path_pc_ = pc+4.
//       Schedule flush at T+2 (when branch reaches EX: IF→ID→EX).
//
//  T+1: IFU issues wrong-path fetch at wrong_path_pc_ (pc+4).
//       This pollutes the iCache, matching RTL behavior.
//
//  T+2: Branch reaches EX. do_execute() triggers flush:
//       - Flush all IFQ entries
//       - Clear wrong-path state
//       - Set fetch_resume_tick_ = T+2 + BranchMissPenalty
//
//  T+2+penalty: First correct-path fetch issues to iCache.
//
void
Pipeline::try_issue_fetch() {
  while (imem->is_ready().first
         && fetch_queue_.size() < ifq_size_) {
    TransPtr candidate = nullptr;

    if (is_draining_) [[unlikely]] {
      Inst drain_inst{0, 0, 0, false, false, 0, {0, 0}, 0, 0};
      candidate =
        std::make_unique<Transaction>(drain_inst, true, true);
    } else if (in_wrong_path_) {
      // IFU keeps fetching wrong-path PCs until EX flushes
      Inst wp_inst{wrong_path_pc_, 0, 0, false, false,
                   0, {0, 0}, 0, 0};
      candidate =
        std::make_unique<Transaction>(wp_inst, true, true);
      wrong_path_pc_ += 4;
      DPRINTF(Pipeline, " IF WrongPath PC=0x%08x", wp_inst.pc);
    } else if (curr_tick() < fetch_resume_tick_) {
      // Post-flush recovery: can't fetch until redirect completes
      break;
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

      if (!accurate) {
        // Misprediction detected at IF. Enter wrong-path mode.
        // IFU will fetch wrong PCs until branch reaches EX.
        in_wrong_path_ = true;
        flush_stall_cycles(curr_tick());
        in_br_recovery_ = true;

        // Wrong-path PC: the PC that IFU would fetch next
        if (real_taken && !pred.will_redirect) {
          // Predicted not-taken but should take: IFU fetches pc+4
          wrong_path_pc_ = inst.pc + 4;
        } else if (!real_taken && pred.will_redirect) {
          // Predicted taken but should not: IFU fetches target
          wrong_path_pc_ = pred.pred_target;
        } else {
          // Wrong target
          wrong_path_pc_ = pred.pred_target;
        }

        // flush_at_tick_ is set but the actual flush happens when
        // the branch passes through do_execute(). The wrong-path
        // state is cleared there.
        DPRINTF(Pipeline,
                " IF BrMispred PC=0x%08x -> WrongPath @0x%08x",
                inst.pc, wrong_path_pc_);
      }
    } else {
      break;
    }

    // Send iCache read request
    [[maybe_unused]] auto const [rdy, _] = imem->is_ready();
    assert(rdy);
    imem->read_req(candidate->trace_inst.pc);
    fetch_queue_.emplace_back(std::move(candidate));
  }
}

// ── Fetch stage 1: IFQ head → pipeline[Fetch] ───────────────────
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
    if (soc_mode_) {
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

// ── iCache response handler ──────────────────────────────────────
void
Pipeline::handle_ifu_resp() {
  // Orphan response: iCache request was in-flight when EX flushed
  if (orphan_icache_resps_ > 0) {
    orphan_icache_resps_--;
    DPRINTF(Pipeline, " IF Resp -> Orphan (ignored), %lu remain",
            orphan_icache_resps_);
    return;
  }
  auto it = std::ranges::find_if(
    fetch_queue_,
    [](const auto& item) { return item->wait_mem; });
  assert(it != fetch_queue_.end());
  (*it)->wait_mem = false;
  DPRINTF(Pipeline, " IF Resp -> PC %08x WP=%d T@ %lu",
          (*it)->trace_inst.pc, (*it)->is_wrong_path,
          curr_tick() + 1);
  async_schedule(Fetch, curr_tick() + 1);
}

// ── Decode ───────────────────────────────────────────────────────
void
Pipeline::do_decode() {
  const auto& trans = sim_pipe_.at(Fetch);
  const auto& inst = trans->trace_inst;

  bool lsu_active =
    sim_pipe_.at(Execute) && sim_pipe_.at(Execute)->wait_mem;

  auto ready_time = std::max(reg_ready_.at(inst.src_reg[0]),
                             reg_ready_.at(inst.src_reg[1]));
  if (ready_time == InfTime) {
    DPRINTF(Pipeline, " ID Blocked: PC=0x%08x src[%d,%d]",
            inst.pc, inst.src_reg[0], inst.src_reg[1]);
    if (!lsu_active)
      set_stall(RAW);
    schedule(Decode, InfTime);
    return;
  } else if (ready_time > curr_tick()) {
    if (!lsu_active)
      set_stall(RAW);
    DPRINTF(Pipeline, " ID Stall until T@%lu: PC=0x%08x",
            ready_time, inst.pc);
    schedule(Decode, ready_time);
    return;
  }
  auto finish_time = std::max(curr_tick(), ready_time) + 1;
  if (!lsu_active)
    set_stall(NoInst);
  schedule(Decode, finish_time);
  if (inst.dst_reg)
    reg_ready_.at(inst.dst_reg) = InfTime;

  DPRINTF(Pipeline,
          " ID Pass PC=0x%08x src[%d,%d] dst=%d T@%lu->%lu",
          inst.pc, inst.src_reg[0], inst.src_reg[1],
          inst.dst_reg, curr_tick(), finish_time);
  sim_pipe_.at(Decode) = std::move(sim_pipe_.at(Fetch));
}

// ── Execute ──────────────────────────────────────────────────────
void
Pipeline::do_execute() {
  assert(stage_update_.at(Decode) <= curr_tick());
  const auto& trans = sim_pipe_.at(Decode);
  const auto& inst = trans->trace_inst;

  // Branch resolution at EX — matches RTL flushWire
  if (inst.is_branch) {
    bool real_taken = inst.br_taken != 0;
    addr_t real_target = real_taken ? inst.mem_addr : 0;
    bpu->update(inst.pc, real_taken, real_target);

    if (trans->br_mispred) {
      // ─── EX-stage flush ──────────────────────────────────
      DPRINTF(Pipeline,
              " EX Flush PC=0x%08x taken=%d target=0x%08x",
              inst.pc, real_taken, real_target);

      // Flush fetch queue — wrong-path entries discarded,
      // real entries (shouldn't exist) get ongoing_insts_--
      // Track in-flight iCache requests that will arrive later
      for (auto& fq_entry : fetch_queue_) {
        if (fq_entry) {
          if (fq_entry->wait_mem)
            orphan_icache_resps_++;
          if (!fq_entry->is_wrong_path)
            ongoing_insts_--;
        }
      }
      fetch_queue_.clear();
      // Clear wrong-path state
      in_wrong_path_ = false;
      // Keep input_buffer_ — it holds a valid trace instruction
      // that should be re-fetched after the recovery stall.

      // After flush, IFU needs BranchMissPenalty cycles to
      // redirect and issue first correct-path fetch
      fetch_resume_tick_ = curr_tick() + BranchMissPenalty;

      // End branch-recovery attribution at EX flush.  RTL only
      // counts the flush+redirect cycles as BranchMispred; any
      // subsequent iCache stall is NoInst.
      flush_stall_cycles(curr_tick() + BranchMissPenalty);
      in_br_recovery_ = false;
    }
  }

  DPRINTF(Pipeline, " EX -> PC=0x%08x", inst.pc);

  // Register forwarding — matches RTL:
  // EXU: gprFw=false (no forwarding from EX)
  // LSU: gprFw=(wbSel==fromAlu) — ALU results forward at LS
  // WBU: always forwards
  if (inst.mem_op == MemNone) {
    if (inst.is_branch && inst.dst_reg != 0) {
      // Jal/Jalr: wbSel!=fromAlu → no LS forward, WB forwards
      update_reg_time(inst.dst_reg, curr_tick() + 2);
    } else {
      // ALU: wbSel=fromAlu → LS forwards (1 cycle after EX)
      update_reg_time(inst.dst_reg, curr_tick() + 1);
    }
  }

  sim_pipe_.at(Execute) = std::move(sim_pipe_.at(Decode));
  schedule(Execute, curr_tick() + 1);
}

// ── Memory ───────────────────────────────────────────────────────
void
Pipeline::do_memory() {
  const auto& trans = sim_pipe_.at(Execute);
  const auto& inst = trans->trace_inst;

  if (trans->wait_mem) {
    schedule(Memory, InfTime);
    return;
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
  // No memory op — pass through
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
          inst.mem_op == MemLoad ? "Load" : "Store", inst.pc,
          inst.mem_addr);

  set_stall(NoInst);
  if (inst.mem_op != MemNone)
    update_reg_time(inst.dst_reg, curr_tick() + 1);

  // Check if decode is blocked by RAW from this load
  if (auto* ids = sim_pipe_.at(Fetch).get()) {
    const auto& idi = ids->trace_inst;
    auto rdy = std::max(reg_ready_.at(idi.src_reg[0]),
                        reg_ready_.at(idi.src_reg[1]));
    if (rdy > curr_tick())
      set_stall(RAW);
  }
  async_schedule(Memory, curr_tick() + 1);
  sim_pipe_.at(Memory) = std::move(sim_pipe_.at(Execute));
}

// ── WriteBack ────────────────────────────────────────────────────
void
Pipeline::do_writeback() {
  ongoing_insts_--;
  DPRINTF(Pipeline, " WB PC=0x%08x Remain %lu",
          sim_pipe_.at(Memory)->trace_inst.pc, ongoing_insts_);
  flush_stall_cycles(curr_tick());
  stats.nostall++;
  last_attr_tick_ = curr_tick() + 1;
  if (sim_pipe_.at(Execute) && sim_pipe_.at(Execute)->wait_mem)
    stall_cause_ = LsuStall;
  else
    stall_cause_ = NoInst;

  stats.insts++;
  stats.cycles = curr_tick() - reset_tick_;
  sim_pipe_.at(Memory) = nullptr;
  schedule(WriteBack, curr_tick() + 1);
}

// ── Mem response dispatch ────────────────────────────────────────
void
Pipeline::recv_mem_resp(CpuTrans trans) {
  auto id = trans.id;
  [[maybe_unused]] auto addr = trans.addr;
  [[maybe_unused]] auto is_write =
    trans.mop == MemRWOpt::Write;
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
  else if (id == 1)
    async_schedule(Memory, curr_tick());
  else
    assert(false && "No such ID");
}

// ── Register ready time update ───────────────────────────────────
void
Pipeline::update_reg_time(uint8_t rd, tick_t when) {
  if (rd)
    reg_ready_.at(rd) = when;
  // Retry decode if it was blocked on this register
  if (auto* ids = sim_pipe_.at(Fetch).get()) {
    if (stage_update_.at(Decode) == InfTime) {
      const auto& idi = ids->trace_inst;
      if (idi.src_reg[0] || idi.src_reg[1]) {
        auto ready_time = std::max(
          reg_ready_.at(idi.src_reg[0]),
          reg_ready_.at(idi.src_reg[1]));
        schedule(Decode, ready_time);
      }
    }
  }
  assert(reg_ready_.at(0) == 0);
}

} // namespace pipeSim
