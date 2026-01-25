#include <iostream>
#include <fstream>
#include <cstdint>
#include <vector>
#include <cstring>

using word_t = uint32_t;

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

int main() {
    std::ofstream out("tests/test.trace", std::ios::binary);
    std::vector<TraceInst> trace;

    // Helper lambda to clear and push
    auto add_inst = [&](word_t pc, uint8_t is_br, uint8_t taken, 
                        std::vector<uint8_t> dst_regs, std::vector<uint8_t> src_regs,
                        std::vector<word_t> dst_mem, std::vector<word_t> src_mem) {
        TraceInst t;
        std::memset(&t, 0, sizeof(t));
        t.pc = pc;
        t.is_branch = is_br;
        t.br_taken = taken;
        if (!dst_regs.empty()) t.dst_reg = dst_regs[0];
        for(size_t i=0; i<src_regs.size() && i<2; ++i) t.src_reg[i] = src_regs[i];
        if (!dst_mem.empty()) t.dst_mem = dst_mem[0];
        if (!src_mem.empty()) t.src_mem = src_mem[0];
        
        std::cout << "Inst: PC=" << std::hex << t.pc << " DstReg=" << (int)t.dst_reg 
                  << " DstMem=" << t.dst_mem << std::dec << " OffsetOfDstReg=" << offsetof(TraceInst, dst_reg) << "\n";
        
        trace.push_back(t);
    };

    // 1. ALU: x1 = x2 + x3
    add_inst(0x1000, 0, 0, {1}, {2, 3}, {}, {});
    
    if (trace.size() > 1) {
       std::cout << "DEBUG: stride=" << (long)((char*)&trace[1] - (char*)&trace[0]) << "\n";
    }

    // 2. LD: x4 = [0x2000] (Assume x1 held addr, but trace records effective addr)
    add_inst(0x1004, 0, 0, {4}, {1}, {}, {0x2000});

    // 3. ALU: x5 = x4 + 1 (RAW on x4, Load Use Hazard)
    add_inst(0x1008, 0, 0, {5}, {4}, {}, {});

    // 4. BR: if x5==0 goto 0x1014 (Taken)
    add_inst(0x100C, 1, 1, {}, {5}, {}, {});

    // 5. ALU: Target (at 0x1014)
    add_inst(0x1014, 0, 0, {6}, {}, {}, {});

    std::cout << "DEBUG: sizeof(TraceInst)=" << sizeof(TraceInst) << "\n";
    std::cout << "Offset dst_reg: " << offsetof(TraceInst, dst_reg) << "\n";
    std::cout << "Offset dst_mem: " << offsetof(TraceInst, dst_mem) << "\n";

    out.write(reinterpret_cast<char*>(trace.data()), trace.size() * sizeof(TraceInst));
    out.close();
    std::cout << "Generated tests/test.trace (" << trace.size() << " insts)\n";
    return 0;
}
