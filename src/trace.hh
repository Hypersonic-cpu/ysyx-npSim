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
}
#endif
