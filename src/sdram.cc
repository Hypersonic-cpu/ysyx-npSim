#include "sdram.hh"
#include <algorithm>

tick_t SDRAM::request_access(addr_t addr, bool is_first, bool is_lsu) {
    auto current_time = curr_tick();
    auto req_latency = is_first ? latency_ : burst_latency_;
    
    // Memory arbiter: if memory is free, grant immediately
    // Otherwise, must wait until busy_until_
    tick_t start_tick = std::max(current_time, busy_until_);
    tick_t end_tick = start_tick + req_latency;
    
    // Update busy time
    busy_until_ = end_tick;
    
    DPRINTF(Sdram, "SDRAM %s @ 0x%08x: %lu -> %lu (lat=%u)",
            is_lsu ? "LSU" : "IF ", addr, start_tick, end_tick, req_latency);
            
    return end_tick;
}
