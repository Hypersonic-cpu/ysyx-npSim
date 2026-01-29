#pragma once

#include "types.hh"
#include "base.hh"
#include "debug.hh"

// SDRAM arbiter: single channel, LSU priority
// When both IF and LSU request at the same time when memory becomes free, LSU wins
class SDRAM {
public:
    SDRAM(tint_t lat, tint_t bst_lat) 
        : latency_(lat), burst_latency_(bst_lat), busy_until_(0) {}

    // Request access from IF or LSU
    // Returns the tick when this access completes
    tick_t request_access(addr_t addr, bool is_first, bool is_lsu);

    // Check when the SDRAM will be free
    tick_t next_free() const { return busy_until_; }

private:
    tint_t latency_;
    tint_t burst_latency_;
    tick_t busy_until_; // When current access finishes
};
