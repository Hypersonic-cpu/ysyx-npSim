#include "cacheSim/CacheBase.hh"
#include "cacheSim/RamConn.hh"
#include "pmem.hh"
#include "types.hh"
#include <iostream>
#include <vector>
#include <cassert>
#include <memory>
#include <map>
#include <iomanip>
#include <cstring>

// Global simulation time
static tick_t current_time = 0;
tick_t curr_tick() noexcept {
    return current_time;
}

// Tester state for avail_handler mechanism
static tick_t tester_next_update = InfTime;
static bool tester_waiting_for_cache = false;  // True if suspended due to cache busy

// Response tracking
static std::map<addr_t, word_t> last_read_data;
static std::map<addr_t, tick_t> read_response_time;
static std::vector<std::pair<tick_t, addr_t>> response_log;

void read_callback(addr_t addr, word_t ret) {
    last_read_data[addr] = ret;
    read_response_time[addr] = current_time;
    response_log.push_back({current_time, addr});
    // Only print first few responses to avoid spam
    if (response_log.size() <= 10 || response_log.size() % 100 == 0) {
        std::cout << "[T=" << std::setw(5) << current_time << "] Response: Addr=0x" 
                  << std::hex << addr << " Data=0x" << ret << std::dec << std::endl;
    }
}

void write_callback(addr_t addr) {
    std::cout << "[T=" << std::setw(5) << current_time << "] Write Response: Addr=0x" 
              << std::hex << addr << std::dec << std::endl;
}

// Avail handler - called by cache when it becomes ready
void avail_handler() {
    // Wake up tester to retry ONLY if it was waiting for cache
    if (tester_waiting_for_cache && tester_next_update == InfTime) {
        tester_next_update = current_time;  // Retry immediately
        tester_waiting_for_cache = false;
    }
}

// Proper event-driven simulation step
void step(std::vector<ClockedObject*>& objects, int cycles = 1) {
    for (int i = 0; i < cycles; ++i) {
        // (I) Find minimum next_update() time (including tester)
        tick_t next_tick = tester_next_update;
        for (auto* obj : objects) {
            tick_t obj_next = obj->next_update();
            if (obj_next < next_tick) {
                next_tick = obj_next;
            }
        }
        
        // (II) Forward time to that tick
        if (next_tick == InfTime) {
            // No pending events, advance by 1
            current_time++;
        } else if (next_tick <= current_time) {
            // Event is ready now, don't advance time
        } else {
            // Advance to next event
            current_time = next_tick;
        }
        
        // (III) Call do_update to all clocked objects in order: mem -> cache
        for (auto* obj : objects) {
            if (obj->next_update() <= current_time) {
                obj->do_update();
            }
        }
    }
}

bool wait_for_ready(cacheSim::CacheBase* cache, std::vector<ClockedObject*>& objects, int max_cycles = 100) {
    for (int i = 0; i < max_cycles; ++i) {
        auto [r_ready, w_ready] = cache->is_ready();
        if (r_ready) return true;
        step(objects, 1);
    }
    return false;
}

void analyze_timing(const std::vector<std::pair<tick_t, addr_t>>& log) {
    if (log.size() < 2) return;
    
    std::cout << "\n=== Timing Analysis ===" << std::endl;
    std::cout << "Total responses: " << log.size() << std::endl;
    
    std::vector<tick_t> intervals;
    for (size_t i = 1; i < log.size(); ++i) {
        tick_t interval = log[i].first - log[i-1].first;
        intervals.push_back(interval);
    }
    
    // Count single-cycle responses (hits)
    int single_cycle = 0;
    int multi_cycle = 0;
    for (auto interval : intervals) {
        if (interval == 1) single_cycle++;
        else multi_cycle++;
    }
    
    std::cout << "Single-cycle intervals (hits): " << single_cycle << std::endl;
    std::cout << "Multi-cycle intervals (after miss): " << multi_cycle << std::endl;
    
    // Show intervals
    std::cout << "\nInterval distribution:" << std::endl;
    for (size_t i = 0; i < std::min(size_t(20), intervals.size()); ++i) {
        std::cout << "  [" << i << "] " << intervals[i] << " cycles" << std::endl;
    }
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [OPTIONS]\n"
              << "Options:\n"
              << "  --assoc <1|2|4>    Set cache associativity (default: 1)\n"
              << "  --help             Show this help\n";
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    int assoc = 1;  // Default
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--assoc") == 0 && i + 1 < argc) {
            assoc = std::atoi(argv[i + 1]);
            if (assoc != 1 && assoc != 2 && assoc != 4) {
                std::cerr << "Error: assoc must be 1, 2, or 4\n";
                return 1;
            }
            ++i;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }
    
    std::cout << "==============================================\n";
    std::cout << "     Cache Timing Test (Associativity=" << assoc << ")\n";
    std::cout << "==============================================\n\n";
    
    // Configuration
    const size_t CACHE_SIZE = 4096;
    const size_t LINE_SIZE = 64;
    const size_t PIPE_DEPTH = 3;
    const tint_t MEM_LATENCY = 50;
    const tint_t BURST_LATENCY = 1;
    
    // Create cache with specified associativity
    auto cache = std::make_unique<cacheSim::PipeCache>(
        "L1Cache", PIPE_DEPTH, CACHE_SIZE, LINE_SIZE, assoc, nullptr, 0
    );
    cache->set_resp_handlers(read_callback, write_callback);
    cache->set_avail_handler(avail_handler);  // Set avail handler
    
    // Create RAM arbiter
    std::vector<cacheSim::CacheBase*> caches = { cache.get() };
    auto ram = std::make_unique<memSim::RAMArbiter>(
        "RAM", MEM_LATENCY, BURST_LATENCY, caches
    );
    cache->set_mem_port(ram.get());
    
    std::vector<ClockedObject*> objects = { cache.get(), ram.get() };
    
    // Setup memory with pattern
    std::cout << "Setting up memory..." << std::endl;
    for (int i = 0; i < 100; ++i) {
        pmem_write(0x1000 + i * 4, 0xA0000000 + i, 0xF);
    }
    
    // TEST 1: Continuous hits (most hits, few misses)
    std::cout << "\n========================================" << std::endl;
    std::cout << "TEST 1: Continuous Hits Pattern" << std::endl;
    std::cout << "Expected: 1 response/cycle for hits," << std::endl;
    std::cout << "          ~" << (PIPE_DEPTH + MEM_LATENCY + (LINE_SIZE/4-1)*BURST_LATENCY) 
              << " cycle stall on miss" << std::endl;
    std::cout << "========================================\n" << std::endl;
    
    response_log.clear();
    current_time = 0;
    tester_next_update = 0;  // Initialize tester
    
    // First, warm up cache line at 0x1000
    std::cout << "Warming up cache line at 0x1000..." << std::endl;
    bool warmup_issued = false;
    while (!read_response_time.count(0x1000) && current_time < 300) {
        // (IV) Tester logic - execute BEFORE step
        if (!warmup_issued && tester_next_update <= current_time) {
            auto [r_ready, w_ready] = cache->is_ready();
            if (r_ready) {
                cache->read_req(0x1000);
                warmup_issued = true;
                tester_next_update = InfTime;  // Wait for response
                tester_waiting_for_cache = false;
            } else {
                tester_next_update = InfTime;  // Wait for avail_handler
                tester_waiting_for_cache = true;
            }
        }
        
        // (I-III) Advance simulation
        step(objects, 1);
    }
    std::cout << "Warmup complete at cycle " << current_time << "\n" << std::endl;
    
    response_log.clear();
    tick_t test_start = current_time;
    
    // Now access pattern: mostly hits in 0x1000-0x1040 (1 cache line)
    // with occasional miss to 0x2000 (different line)
    std::cout << "Starting continuous access pattern..." << std::endl;
    std::vector<addr_t> access_pattern;
    
    // Generate pattern: 15 hits, 1 miss, repeat
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 15; ++i) {
            access_pattern.push_back(0x1000 + (i % 16) * 4);  // Within same line
        }
        access_pattern.push_back(0x2000 + round * 64);  // Miss to different line
    }
    
    // Issue all requests using avail_handler mechanism
    size_t issued = 0;
    tester_next_update = current_time;  // Start issuing
    tester_waiting_for_cache = false;
    int loop_count = 0;
    
    while (issued < access_pattern.size() || response_log.size() < access_pattern.size()) {
        loop_count++;
        if (loop_count == 100 || loop_count % 1000 == 0) {
            std::cout << "[Loop " << loop_count << "] time=" << current_time 
                      << " issued=" << issued << "/" << access_pattern.size()
                      << " responses=" << response_log.size() << std::endl;
        }
        
        // (IV) Tester attempts to send requests to cache (BEFORE step)
        if (tester_next_update <= current_time && issued < access_pattern.size()) {
            auto [r_ready, w_ready] = cache->is_ready();
            if (r_ready) {
                // Cache is ready, issue request
                cache->read_req(access_pattern[issued]);
                issued++;
                // Try to issue next request immediately next cycle
                tester_next_update = current_time + 1;
                tester_waiting_for_cache = false;
            } else {
                // Cache is busy, suspend tester until avail_handler wakes us
                tester_next_update = InfTime;
                tester_waiting_for_cache = true;
            }
        }
        
        // (I-III) Advance simulation
        step(objects, 1);
        
        // Safety timeout
        if (current_time > test_start + 2000) break;
    }
    
    tick_t test_end = current_time;
    std::cout << "\nTest completed in " << (test_end - test_start) << " cycles" << std::endl;
    std::cout << "Responses received: " << response_log.size() << "/" << access_pattern.size() << std::endl;
    
    analyze_timing(response_log);
    
    // Verify we see 1 hit/cycle pattern
    bool timing_correct = true;
    int consecutive_singles = 0;
    int max_consecutive = 0;
    
    for (size_t i = 1; i < response_log.size(); ++i) {
        tick_t interval = response_log[i].first - response_log[i-1].first;
        if (interval == 1) {
            consecutive_singles++;
            max_consecutive = std::max(max_consecutive, consecutive_singles);
        } else {
            consecutive_singles = 0;
        }
    }
    
    std::cout << "\nMax consecutive 1-cycle responses: " << max_consecutive << std::endl;
    if (max_consecutive >= 10) {
        std::cout << "✓ PASS: Observed sustained 1 response/cycle for hits" << std::endl;
    } else {
        std::cout << "✗ FAIL: Did not observe sustained 1 response/cycle" << std::endl;
        timing_correct = false;
    }
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "Final Cache Statistics" << std::endl;
    std::cout << "========================================" << std::endl;
    cache->dump_stats();
    
    std::cout << "\n==============================================" << std::endl;
    if (timing_correct) {
        std::cout << "     ✓ TIMING TEST PASSED" << std::endl;
    } else {
        std::cout << "     ✗ SOME TIMING TESTS FAILED" << std::endl;
    }
    std::cout << "==============================================" << std::endl;
    
    return timing_correct ? 0 : 1;
}
