#pragma once

#ifdef __cplusplus
#include <cstdint>
#include <string>

namespace trace {
#else
#include <stdint.h>
#endif

/** NEMU visible */
enum MemOp { MemNone = 0, MemLoad = 1, MemStore = 2 };
enum SysOp { SysNone = 0, SysResetStats = 1, SysDumpStats = 2 };

#pragma pack(push, 1)
struct TraceInst {
  /*  3: 0 */ uint32_t pc;
  /*  7: 4 */ uint32_t mem_addr;  // load/store addr, or branch target if taken
  /*     8 */ uint8_t mem_op;     // 0: No Mem Op, 1: Load, 2: Store
  /*     9 */ uint8_t is_branch;  // 0: No, 1: Yes
  /*    10 */ uint8_t br_taken;   // 0: Not Taken, 1: Taken
  /*    11 */ uint8_t dst_reg;    // 0 if unused
  /* 13:12 */ uint8_t src_reg[2]; // 0 if unused
  /*    14 */ uint8_t sys_op;
  /*    15 */ uint8_t dummy;
};
#pragma pack(pop)
/** NEMU visible end */

#ifdef __cplusplus
class TraceReader {
public:
  TraceReader(const std::string& filename);
  ~TraceReader();
  bool next(TraceInst& inst);

private:
  static bool isxz(const std::string& filename);
  // Order matters (Ctor)
  FILE* file_;
};

// Trace sanitizer: counts instruction categories for validation.
// Accumulates per-instruction stats; resettable for stats-windowing.
struct TraceSanitizer {
  size_t total = 0;
  size_t loads = 0;
  size_t stores = 0;
  size_t branches = 0;
  size_t br_taken = 0;
  size_t br_not_taken = 0;
  size_t alu = 0;         // non-branch, non-mem
  size_t has_dst = 0;     // instructions that write a register
  size_t has_src1 = 0;
  size_t has_src2 = 0;
  size_t sys_ops = 0;     // ebreak with sys_op != 0
  // Sanity checks
  size_t bad_marker = 0;  // dummy != 0x73 (corrupt record)
  size_t br_taken_no_target = 0;  // br_taken=1 but mem_addr=0

  void record(const TraceInst& t) {
    total++;
    if (t.mem_op == MemLoad) loads++;
    else if (t.mem_op == MemStore) stores++;
    if (t.is_branch) {
      branches++;
      if (t.br_taken) {
        br_taken++;
        if (t.mem_addr == 0) br_taken_no_target++;
      } else {
        br_not_taken++;
      }
    }
    if (t.mem_op == MemNone && !t.is_branch) alu++;
    if (t.dst_reg != 0) has_dst++;
    if (t.src_reg[0] != 0) has_src1++;
    if (t.src_reg[1] != 0) has_src2++;
    if (t.sys_op != SysNone) sys_ops++;
    if (t.dummy != 0x73) bad_marker++;
  }

  void reset() { *this = TraceSanitizer{}; }

  void dump() const;
};

}
#endif
