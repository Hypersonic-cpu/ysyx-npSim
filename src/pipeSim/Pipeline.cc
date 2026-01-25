#include "Pipeline.hh"
#include "debug.hh"
#include "trace.hh"
#include "types.hh"
#include <algorithm>

namespace pipeSim {

/**
 * Processing follow the instruction order. Thus start from PCGEN to WB,
 * unlike the backward approach used in gem5.
 */
void
Pipeline::iota_inst(const trace::TraceInst& inst, tint_t fetch_lat,
                    tint_t load_lat, tint_t store_lat, bool is_mispred) {
  stats.insts++;

  DPRINTF(Pipeline, "PC=0x%x FetchLat=%u DataLat(Ld:St)=(%u:%u) Mispred=%d",
          inst.pc, fetch_lat, load_lat, store_lat, is_mispred);

  // 1. IF Stage
  fetch_queue_.auto_dequeue(start_tick_);
  auto fetch_avail = start_tick_;
  if (fetch_queue_.is_full()) {
    fetch_avail = std::max(fetch_avail, fetch_queue_.next_avaiable());
    fetch_queue_.auto_dequeue(fetch_avail);
    DPRINTF(IFQueue, "  IFQ Full, next avail @T %lu", fetch_avail);
  }
  auto fetch_end = fetch_avail + fetch_lat;
  auto fetch_stalls = fetch_avail - start_tick_;
  start_tick_ = fetch_avail + 1;
  fetch_queue_.enqueue(fetch_end, inst.pc);

  stats.stalls += fetch_stalls;
  DPRINTF(Pipeline, "  Fetch: %lu -> %lu", start_tick_, fetch_end);

  // 2. ID Stage
  tick_t decode_end = fetch_end + 1;

  // 3. EX Stage
  // Can start after decode_end and after previous exec is done
  // AND after operands are ready
  tick_t operand_ready = 0;
  // RAW hazard check
  for (int i = 0; i < 2; ++i) {
    operand_ready = std::max(operand_ready, reg_ready_[inst.src_reg[i]]);
  }

  tick_t exec_start0 = decode_end + 1;
  tick_t exec_start1 = std::max(exec_start0, operand_ready);
  tick_t exec_end = exec_start1 + 1;

  // Stall accounting (if wait for operands > wait for pipeline slot)
  if (exec_start1 > exec_start0) {
    tick_t stall_cycles = (exec_start1 - exec_start0);
    stats.stalls += stall_cycles;
    stats.raw_stalls += stall_cycles;
    DPRINTF(Pipeline, "  Stall RAW: %lu cycles (RegReady=%lu, Normal=%lu)",
            stall_cycles, operand_ready, exec_start0);
  }
  DPRINTF(Pipeline, "  Exec: %lu -> %lu", exec_start0, exec_end);

  // 4. MEM Stage
  bool is_load = inst.mem_op == trace::MemOp::Load;
  bool is_store = inst.mem_op == trace::MemOp::Store;
  tint_t mem_duration = 1;
  tick_t mem_avail = exec_end;
  memst_queue_.auto_dequeue(exec_end);
  if (is_load) {
    if (memst_queue_.contains(inst.mem_addr)) {
      // Hit buffer, 1 cycle lat
      // TODO: Only word read/write can hit buffer.
      DPRINTF(LDQueue, "  load @ %x buffer hit", inst.mem_addr);
    } else {
      mem_duration = load_lat;
      DPRINTF(LDQueue, "  load @ %x buffer miss, finish @T %lu",
              inst.mem_addr, mem_avail + mem_duration);
    }
  } else if (is_store) {
    // TODO: coalesce multiple store to the same addr
    mem_duration = store_lat;
    if (memst_queue_.is_full()) {
      mem_avail = std::max(mem_avail, memst_queue_.next_avaiable());
      memst_queue_.auto_dequeue(mem_avail);
      DPRINTF(STQueue, "  store buffer full, next avail @T %lu", mem_avail);
    }
    memst_queue_.enqueue(exec_end, inst.mem_addr);
  }
  tick_t mem_end = mem_avail + mem_duration;
  DPRINTF(Mem, "  %lu -> %lu", mem_avail, mem_end);

  // 5. WB Stage
  tick_t wb_end = mem_end + 1;
  stats.cycles = wb_end; // Update total cycles

  /** Overwrite */
  // Forwarding / Register Update
  if (inst.dst_reg != 0) {
    uint8_t rd = inst.dst_reg;
    // 1 cycle lat for forward.
    if (is_load) {
      reg_ready_[rd] = mem_end;
    } else {
      reg_ready_[rd] = exec_end;
    }
    DPRINTF(Pipeline, "  RegUpd: x%d ready @T %lu", rd, reg_ready_[rd]);
  }

  // Branch misprediction: next instruction fetch delayed
  if (inst.is_branch && is_mispred) {
    // [IF 1 2 3] [ID] [EX]
    //                     [ IF 1 2 3 ]
    // flushed -----------|
    auto next_start = exec_end + 1;
    DPRINTF(Pipeline, "  Branch MisPred: Next fetch delayed from %lu to %lu",
            start_tick_, next_start);
    start_tick_ = next_start;
    stats.flush_count++;
  }
}
} // namespace pipeSim
