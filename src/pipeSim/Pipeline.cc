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
                   BranchUnit* bpu)
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
    , penalty_inst_queue_{}
    , ongoing_insts_{0} {
  assert(bpu && "BranchUnit must not be null");
}

/**
 * Processing follow the instruction order. Thus start from PCGEN to WB,
 * unlike the backward approach used in gem5.
 */
void
Pipeline::update_impl() {
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
  // std::print("Next Core Update (");
  // for (auto elem : stage_update_) {
  //   std::print("{:d}, ", (int64_t)elem);
  // }
  // std::println(")");
}

void
Pipeline::do_fetch_0() {
  if (!imem->is_ready().first || fetch_inst_queue_.size() == ifq_size_) {
    return;
  }
  DPRINTF(Cache, "ICache ready = [%d, %d] InstQue size = %lu",
          imem->is_ready().first, imem->is_ready().second,
          fetch_inst_queue_.size());

  TransPtr candidate = nullptr;
  // Process pending penalty fetches first
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
  } else if (!penalty_inst_queue_.empty()) {
    addr_t penalty_pc = penalty_inst_queue_.front();
    penalty_inst_queue_.pop();

    // Create dummy instruction (add x0, x0, x0)
    Inst penalty_inst{/* pc        */ penalty_pc,
                      /* mem_addr  */ 0,
                      /* mem_op    */ 0,
                      /* is_branch */ false,
                      /* br_taken  */ false,
                      /* dst_reg   */ 0,
                      /* src_reg   */ {0, 0},
                      /* sys_op    */ 0,
                      /* dummy     */ 0};

    // Don't call BPU predict/judge for penalty fetches for convenience.
    // They're just consuming cache/memory bandwidth.
    auto penalty_trans = std::make_unique<Transaction>(
      penalty_inst, /* is_penalty */ true, /* wait_mem */ true);
    candidate = std::move(penalty_trans);
  } else if (input_buffer_ != nullptr) {
    candidate = std::move(input_buffer_);
    candidate->wait_mem = true;
  } else {
    return;
  }

  const auto& inst = candidate->trace_inst;

  // Branch prediction at IF stage (before knowing if it's actually a
  // branch). Only predict on non-penalty insts to avoid repetitive
  // punishment.
  if (!candidate->is_penalty_fetch) {
    auto pred = bpu->predict(inst.pc);
    auto real_taken = inst.is_branch && inst.br_taken;
    addr_t real_target = real_taken ? inst.mem_addr : 0;
    // Use BranchUnit::judge to check accuracy and update stats
    auto accurate = bpu->judge(real_taken, real_target, pred);
    candidate->br_pred = pred;
    candidate->br_mispred = !accurate;

    if (!accurate) {
      // Generate penalty fetches for the wrong path
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

      auto penalty_seq = std::views::iota(0U, PenaltyFetchCount - 1U)
                         | std::views::transform(
                           [=](int i) { return i * 4 + wrong_path_pc; });
      DPRINTF(
        Pipeline,
        " IF BrPred Wrong -> Enqueue %lu penalty fetchs @PC=0x%08x ...",
        PenaltyFetchCount, wrong_path_pc);

      penalty_inst_queue_.push_range(penalty_seq);
    }
  }

  send_ifu_req(candidate->trace_inst.pc);
  fetch_inst_queue_.emplace_back(std::move(candidate));
}

void
Pipeline::do_fetch_1() {
  assert(fetch_inst_queue_.size() <= ifq_size_);
  // auto& nxtupd = stage_update_.at(Fetch);
  if (fetch_inst_queue_.size()) {
    assert(sim_pipe_.at(Fetch) == nullptr);
    auto& ptr = fetch_inst_queue_.front();
    if (ptr->wait_mem) {
      // nxtupd = InfTime;
      schedule(Fetch, InfTime);
      return;
    }
    // ptr->out_valid = true;
    if (!ptr->is_penalty_fetch)
      sim_pipe_.at(Fetch) = std::move(ptr);
    fetch_inst_queue_.pop_front();
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
  DPRINTF(Pipeline, " IF Resp -> PC %08x Penalty %d Sched T@ %lu",
          ptr->trace_inst.pc, ptr->is_penalty_fetch, curr_tick() + 1);
  // stage_update_.at(Fetch) = curr_tick() + 1;
  async_schedule(Fetch, curr_tick() + 1);
}

void
Pipeline::do_decode() {
  const auto& trans = sim_pipe_.at(Fetch);
  const auto& inst = trans->trace_inst;

  auto ready_time =
    std::max(reg_ready_.at(inst.src_reg[0]), reg_ready_.at(inst.src_reg[1]));
  auto rd = inst.dst_reg;
  if (ready_time == InfTime) {
    DPRINTF(Pipeline, " ID -> Blocked : PC=0x%08x src[%d,%d] dst=%d",
            inst.pc, inst.src_reg[0], inst.src_reg[1], inst.dst_reg);
    schedule(Decode, InfTime);
    return;
  } else if (ready_time > curr_tick()) {
    // Track RAW stalls
    // stats.backend_stalls += ready_time - curr_tick();
    // stage_update_.at(Decode) = ready_time;
    DPRINTF(Pipeline, " ID -> Until T@ %lu: PC=0x%08x src[%d,%d] dst=%d",
            ready_time, inst.pc, inst.src_reg[0], inst.src_reg[1],
            inst.dst_reg);
    schedule(Decode, ready_time);
    return;
  }
  // Known time
  auto finish_time = std::max(curr_tick(), ready_time) + 1;
  // stage_update_.at(Decode) = finish_time;
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

  if (inst.mem_op == MemNone) {
    update_reg_time(inst.dst_reg, curr_tick() + 1);
  }

  sim_pipe_.at(Execute) = std::move(sim_pipe_.at(Decode));
  schedule(Execute, curr_tick() + 1);
}

void
Pipeline::do_memory() {
  const auto& trans = sim_pipe_.at(Execute);
  const auto& inst = trans->trace_inst;

  if (inst.mem_op == MemLoad) {
    DPRINTF(Pipeline, "LS -> Req [Load] PC=0x%08x addr=0x%08x", inst.pc,
            inst.mem_addr);
    send_lsu_req(inst.mem_addr, 0xbadU, 0xf, false);
    return;
  } else if (inst.mem_op == MemStore) {
    DPRINTF(Pipeline, "LS -> Req [Store] PC=0x%08x addr=0x%08x", inst.pc,
            inst.mem_addr);
    send_lsu_req(inst.mem_addr, 0xbadU, 0xf, true);
    // stage_update_.at(Memory) = InfTime;
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
  // stage_update_.at(Memory) = InfTime;
  schedule(Memory, InfTime);
  if (is_write ? wready : rready) {
    sim_pipe_.at(Execute)->wait_mem = true;
    auto const aligned = addr & ~0x3U;
    if (is_write) {
      dmem->write_req(aligned, data, strb);
    } else {
      dmem->read_req(aligned);
    }
  } else {
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

  if (inst.mem_op != MemNone) {
    update_reg_time(inst.dst_reg, curr_tick() + 1);
  }
  async_schedule(Memory, curr_tick() + 1);
  sim_pipe_.at(Memory) = std::move(sim_pipe_.at(Execute));
}

void
Pipeline::do_writeback() {
  ongoing_insts_--;
  DPRINTF(Pipeline, " WB -> PC=0x%08x Remain %lu",
          sim_pipe_.at(Memory)->trace_inst.pc, ongoing_insts_);
  stats.insts++;
  stats.cycles = curr_tick();
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
    schedule(Fetch, curr_tick());
    return;
  } else if (id == 1) {
    schedule(Memory, curr_tick());
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

} // namespace pipeSim
