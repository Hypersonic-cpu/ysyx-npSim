#include <iostream>
#include <fstream>
#include <cstdint>
#include <vector>
#include <cstring>

#include "trace.hh"

using word_t = uint32_t;
using TraceInst = trace::TraceInst;

int main() {
    std::ofstream out("tests/test.trace", std::ios::binary);
    std::vector<TraceInst> trace;

    // Helper lambda to clear and push
    auto add_inst = [&](word_t pc, uint8_t is_br, uint8_t taken,
                        std::vector<uint8_t> dst_regs, std::vector<uint8_t> src_regs,
                        std::vector<word_t> dst_mem, std::vector<word_t> src_mem,
                        uint8_t sys_op = 0) {
        TraceInst t;
        std::memset(&t, 0, sizeof(t));
        t.pc = pc;
        t.is_branch = is_br;
        t.br_taken = taken;
        if (!dst_regs.empty()) t.dst_reg = dst_regs[0];
        for(size_t i=0; i<src_regs.size() && i<2; ++i) t.src_reg[i] = src_regs[i];
        if (!dst_mem.empty()) t.mem_addr = dst_mem[0];
        else if (!src_mem.empty()) t.mem_addr = src_mem[0];
        
        t.mem_op = !dst_mem.empty() ? 2 : (!src_mem.empty() ? 1 : 0);
        t.sys_op = sys_op;

        std::cout << "Inst: PC=" << std::hex << t.pc << " DstReg=" << (int)t.dst_reg
                  << " MemAddr=" << t.mem_addr << " SysOp=" << (int)t.sys_op << "\n";

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

    // 6. SysDumpStats
    add_inst(0x1018, 0, 0, {}, {}, {}, {}, 2 /* SysDumpStats */);

    std::cout << "DEBUG: sizeof(TraceInst)=" << sizeof(TraceInst) << "\n";
    std::cout << "Offset dst_reg: " << offsetof(TraceInst, dst_reg) << "\n";
    std::cout << "Offset dst_mem: " << offsetof(TraceInst, dst_mem) << "\n";

    out.write(reinterpret_cast<char*>(trace.data()), trace.size() * sizeof(TraceInst));
    out.close();
    std::cout << "Generated tests/test.trace (" << trace.size() << " insts)\n";
    return 0;
}
