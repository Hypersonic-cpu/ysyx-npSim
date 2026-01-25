#pragma once
#include "types.hh"
#include <cstdint>
#include <fstream>
#include <vector>

namespace trace {

#pragma pack(push, 1)
struct TraceInst {
    word_t pc;
    uint8_t is_branch;      // 0: No, 1: Yes
    uint8_t br_taken;       // 0: Not Taken, 1: Taken
    uint8_t dst_reg;        // 0 if unused
    uint8_t src_reg[2];     // 0 if unused
    word_t dst_mem;         // 0 if unused
    word_t src_mem;         // 0 if unused
};
#pragma pack(pop)

class TraceReader {
public:
    TraceReader(const char* filename);
    bool next(TraceInst& inst);
private:
    std::ifstream file_;
};

}
