#include <iostream>
#include <fstream>
#include <string>
#include <cstdint>
#include <memory>
#include "cacheSim/CacheSimulator.hh"
#include "branchSim/BranchPredictor.hh"

using namespace cacheSim;
using namespace branchSim;

// Trace format:
// M <hexaddr>        memory access
// B <hexpc> <0|1>    branch with actual outcome

int main(int argc, char **argv){
    if(argc < 2){ std::cerr << "Usage: " << argv[0] << " trace_file\n"; return 2; }
    std::ifstream in(argv[1]);
    if(!in){ std::cerr << "Cannot open trace\n"; return 2; }

    CacheSimulator cache(64*1024, 64, 4);
    BimodalPredictor bp(12);

    uint64_t local_tick = 0;

    std::string op;
    addr_t a; int t;
    while(in >> op){
        if(op == "M"){
            in >> std::hex >> a >> std::dec;
            cache.access(a, local_tick);
            cache.handle_prefetch(a, local_tick);
        } else if(op == "B"){
            in >> std::hex >> a >> std::dec >> t;
            bool pred = bp.predict(a);
            bp.update(a, t!=0);
            (void)pred;
        } else {
            std::getline(in, op);
        }
        ++local_tick;
    }

    auto s = cache.stats();
    std::cout << "Cache accesses=" << s.accesses << " hits=" << s.hits << " misses=" << s.misses << "\n";
    std::cout << "Done.\n";
    return 0;
}
