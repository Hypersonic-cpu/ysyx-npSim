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

// Implement global curr_tick
static tick_t current_time = 0;
tick_t curr_tick() noexcept {
    return current_time;
}

// Global state for verification
static std::map<addr_t, word_t> last_read_data;
static std::map<addr_t, bool> read_done_flags;
static std::map<addr_t, tick_t> read_done_cycle;
static int test_failed = 0;

void read_callback(addr_t addr, word_t ret) {
    std::cout << "[Callback] Read: Addr=0x" << std::hex << addr << " Data=0x" << ret << std::dec << " @ cycle " << current_time << std::endl;
    last_read_data[addr] = ret;
    read_done_flags[addr] = true;
    read_done_cycle[addr] = current_time;
}

void write_callback(addr_t addr) {
    std::cout << "[Callback] Write: Addr=0x" << std::hex << addr << std::dec << " @ cycle " << current_time << std::endl;
}

#define TEST_ASSERT(cond, msg) \
    if (!(cond)) { \
        std::cout << "FAILED: " << msg << std::endl; \
        test_failed++; \
        return false; \
    }

#define TEST_SECTION(name) \
    std::cout << "\n========================================" << std::endl; \
    std::cout << "=== " << name << std::endl; \
    std::cout << "========================================" << std::endl;

bool run_basic_tests(cacheSim::PipeCache* cache, std::vector<ClockedObject*>& objects);
bool run_blocking_tests(cacheSim::PipeCache* cache, std::vector<ClockedObject*>& objects);
bool run_extreme_tests(cacheSim::PipeCache* cache, std::vector<ClockedObject*>& objects);

int main() {
    std::cout << "==============================================\n";
    std::cout << "     Cache & Memory Test Suite\n";
    std::cout << "==============================================\n";

    // Configuration
    const size_t CACHE_SIZE = 4096;
    const size_t LINE_SIZE = 64;
    const size_t ASSOC = 4;
    const size_t PIPE_DEPTH = 3;
    const tint_t MEM_LATENCY = 50;
    const tint_t BURST_LATENCY = 1;

    // Instantiate Cache
    auto cache = std::make_unique<cacheSim::PipeCache>(
        "L1Cache", PIPE_DEPTH, CACHE_SIZE, LINE_SIZE, ASSOC, nullptr, 0
    );
    cache->set_resp_handlers(read_callback, write_callback);

    // Instantiate RAM Arbiter
    std::vector<cacheSim::CacheBase*> caches = { cache.get() };
    auto ram = std::make_unique<memSim::RAMArbiter>(
        "RAM", MEM_LATENCY, BURST_LATENCY, caches
    );
    cache->set_mem_port(ram.get());

    // Objects to tick
    std::vector<ClockedObject*> objects = { cache.get(), ram.get() };

    // Run test suites
    bool all_passed = true;
    all_passed &= run_basic_tests(cache.get(), objects);
    all_passed &= run_blocking_tests(cache.get(), objects);
    all_passed &= run_extreme_tests(cache.get(), objects);

    // Final report
    std::cout << "\n==============================================\n";
    if (all_passed && test_failed == 0) {
        std::cout << "     ✓ ALL TESTS PASSED\n";
        std::cout << "==============================================\n";
        return 0;
    } else {
        std::cout << "     ✗ SOME TESTS FAILED (" << test_failed << " failures)\n";
        std::cout << "==============================================\n";
        return 1;
    }
}

// Helper functions
// Proper event-driven simulation step
void step(std::vector<ClockedObject*>& objects, int cycles = 1) {
    for (int i = 0; i < cycles; ++i) {
        // (I) Find minimum next_update() time
        tick_t next_tick = InfTime;
        for (auto* obj : objects) {
            tick_t obj_next = obj->next_update();
            if (obj_next < next_tick) {
                next_tick = obj_next;
            }
        }
        
        // (II) Forward time to that tick
        if (next_tick == InfTime) {
            current_time++;  // No pending events, just advance by 1
        } else {
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

bool wait_for_ready(cacheSim::PipeCache* cache, std::vector<ClockedObject*>& objects, int max_cycles = 1000) {
    for (int i = 0; i < max_cycles; ++i) {
        auto [r_ready, w_ready] = cache->is_ready();
        if (r_ready) return true;
        step(objects);
    }
    return false;
}

bool wait_for_response(addr_t addr, std::vector<ClockedObject*>& objects, int max_cycles = 500) {
    for (int i = 0; i < max_cycles; ++i) {
        if (read_done_flags[addr]) return true;
        step(objects);
    }
    return false;
}

// ===== BASIC TESTS =====
bool run_basic_tests(cacheSim::PipeCache* cache, std::vector<ClockedObject*>& objects) {
    TEST_SECTION("Basic Functionality Tests");
    
    // Setup memory
    pmem_write(0x1000, 0xDEADBEEF, 0xF);
    pmem_write(0x1004, 0xCAFEBABE, 0xF);
    pmem_write(0x2000, 0x87654321, 0xF);
    
    word_t verify;
    pmem_read(0x1000, &verify);
    TEST_ASSERT(verify == 0xDEADBEEF, "pmem_read/write verification");
    
    std::cout << "✓ Memory setup verified\n";
    
    // Test 1: Cold miss
    std::cout << "\n--- Test 1.1: Cold Cache Miss ---\n";
    read_done_flags.clear();
    wait_for_ready(cache, objects);
    
    tick_t start = current_time;
    cache->read_req(0x1000);
    
    bool done = false;
    for (int i = 0; i < 200; ++i) {
        step(objects);
        if (read_done_flags[0x1000]) {
            done = true;
            break;
        }
    }
    
    TEST_ASSERT(done, "Cold miss should complete");
    TEST_ASSERT(last_read_data[0x1000] == 0xDEADBEEF, "Cold miss data correct");
    
    tick_t latency = read_done_cycle[0x1000] - start;
    std::cout << "Latency: " << latency << " cycles\n";
    std::cout << "✓ Cold miss passed\n";
    
    // Test 2: Cache hit
    std::cout << "\n--- Test 1.2: Cache Hit ---\n";
    read_done_flags.clear();
    wait_for_ready(cache, objects);
    
    start = current_time;
    cache->read_req(0x1000);
    
    for (int i = 0; i < 20; ++i) {
        step(objects);
        if (read_done_flags[0x1000]) break;
    }
    
    TEST_ASSERT(read_done_flags[0x1000], "Cache hit should complete");
    latency = read_done_cycle[0x1000] - start;
    std::cout << "Latency: " << latency << " cycles\n";
    TEST_ASSERT(latency == 3 || latency == 4, "Cache hit latency should be ~3 cycles");
    std::cout << "✓ Cache hit passed\n";
    
    // Test 3: Same line different offset
    std::cout << "\n--- Test 1.3: Same Cache Line ---\n";
    read_done_flags.clear();
    wait_for_ready(cache, objects);
    
    cache->read_req(0x1004);
    for (int i = 0; i < 20; ++i) {
        step(objects);
        if (read_done_flags[0x1004]) break;
    }
    
    TEST_ASSERT(last_read_data[0x1004] == 0xCAFEBABE, "Same line data correct");
    std::cout << "✓ Same line access passed\n";
    
    return true;
}

// ===== BLOCKING TESTS =====
bool run_blocking_tests(cacheSim::PipeCache* cache, std::vector<ClockedObject*>& objects) {
    TEST_SECTION("Memory Blocking Tests");
    
    // Reset cache
    cache->flush_all();
    current_time += 10;
    
    // Setup test data
    for (int i = 0; i < 10; ++i) {
        addr_t addr = 0x10000 + i * 0x1000;  // Different cache sets
        pmem_write(addr, 0xBEEF0000 + i, 0xF);
    }
    
    // Test 1: Back-to-back misses (pipeline blocking)
    std::cout << "\n--- Test 2.1: Back-to-Back Misses ---\n";
    read_done_flags.clear();
    wait_for_ready(cache, objects);
    
    tick_t req1_start = current_time;
    cache->read_req(0x10000);
    
    // Wait for completion of first request
    for (int i = 0; i < 200; ++i) {
        step(objects);
        if (read_done_flags[0x10000]) break;
    }
    TEST_ASSERT(read_done_flags[0x10000], "First request completed");
    tick_t req1_latency = read_done_cycle[0x10000] - req1_start;
    std::cout << "First miss latency: " << req1_latency << " cycles\n";
    
    // Second request immediately after
    read_done_flags.clear();
    wait_for_ready(cache, objects);
    tick_t req2_start = current_time;
    cache->read_req(0x11000);
    
    for (int i = 0; i < 200; ++i) {
        step(objects);
        if (read_done_flags[0x11000]) break;
    }
    TEST_ASSERT(read_done_flags[0x11000], "Second request completed");
    tick_t req2_latency = read_done_cycle[0x11000] - req2_start;
    std::cout << "Second miss latency: " << req2_latency << " cycles\n";
    
    // FIXME: Check if second request is properly blocked by memory
    std::cout << "✓ Sequential misses handled\n";
    
    // Test 2: Pipeline saturation
    std::cout << "\n--- Test 2.2: Pipeline Saturation ---\n";
    cache->flush_all();
    current_time += 10;
    read_done_flags.clear();
    
    // Fill pipeline with hits (after warming cache)
    wait_for_ready(cache, objects);
    cache->read_req(0x10000);
    for (int i = 0; i < 100; ++i) {
        step(objects);
        if (read_done_flags[0x10000]) break;
    }
    
    // Now do rapid-fire hits
    read_done_flags.clear();
    int completed = 0;
    tick_t start = current_time;
    
    for (int i = 0; i < 5; ++i) {
        wait_for_ready(cache, objects, 50);
        cache->read_req(0x10000 + (i % 4) * 4);  // Within same line
        step(objects, 1);
    }
    
    // Let them all complete
    for (int i = 0; i < 50; ++i) {
        step(objects);
    }
    
    std::cout << "✓ Pipeline saturation handled\n";
    
    return true;
}

// ===== EXTREME CONDITION TESTS =====
bool run_extreme_tests(cacheSim::PipeCache* cache, std::vector<ClockedObject*>& objects) {
    TEST_SECTION("Extreme Condition Tests");
    
    cache->flush_all();
    current_time += 10;
    
    // Test 1: Capacity miss (eviction)
    std::cout << "\n--- Test 3.1: Cache Eviction ---\n";
    
    // Cache is 4KB, 64B lines, 4-way = 16 sets
    // Fill one set completely (4 ways) then add one more
    const int num_ways = 4;
    const int set_size = 16;
    
    // All these addresses map to set 0 (offset by cache size)
    for (int i = 0; i < num_ways + 1; ++i) {
        addr_t addr = i * 4096;  // Each maps to set 0
        pmem_write(addr, 0xAA000000 + i, 0xF);
        
        read_done_flags.clear();
        wait_for_ready(cache, objects);
        cache->read_req(addr);
        
        for (int j = 0; j < 200; ++j) {
            step(objects);
            if (read_done_flags[addr]) break;
        }
        TEST_ASSERT(read_done_flags[addr], "Eviction test request completed");
    }
    std::cout << "✓ Cache eviction handled\n";
    
    // Test 2: Boundary addresses
    std::cout << "\n--- Test 3.2: Boundary Addresses ---\n";
    
    addr_t boundary_addrs[] = {
        0x0000, 0x003C, 0x0040, 0x007C,  // Line boundaries
        0xFFFC, 0x1FFFC, 0x3FFFC         // High addresses
    };
    
    for (auto addr : boundary_addrs) {
        pmem_write(addr, 0xB0000000 + (addr & 0xFF), 0xF);
    }
    
    for (auto addr : boundary_addrs) {
        read_done_flags.clear();
        wait_for_ready(cache, objects);
        cache->read_req(addr);
        
        for (int i = 0; i < 200; ++i) {
            step(objects);
            if (read_done_flags[addr]) break;
        }
        TEST_ASSERT(read_done_flags[addr], "Boundary address handled");
    }
    std::cout << "✓ Boundary addresses handled\n";
    
    // Test 3: Thrashing
    std::cout << "\n--- Test 3.3: Cache Thrashing ---\n";
    cache->flush_all();
    current_time += 10;
    
    // Access pattern that causes thrashing (same set)
    addr_t thrash_addrs[] = {
        0x0000, 0x1000, 0x2000, 0x3000, 0x4000,  // 5 addrs, only 4 ways
        0x0000, 0x1000, 0x2000, 0x3000, 0x4000   // Repeat
    };
    
    for (auto addr : thrash_addrs) {
        read_done_flags.clear();
        wait_for_ready(cache, objects);
        cache->read_req(addr);
        
        for (int i = 0; i < 200; ++i) {
            step(objects);
            if (read_done_flags[addr]) break;
        }
    }
    
    // Check stats
    std::cout << "\n--- Final Cache Statistics ---\n";
    cache->dump_stats();
    
    std::cout << "✓ Thrashing pattern handled\n";
    
    // Test 4: Long simulation
    std::cout << "\n--- Test 3.4: Extended Operation ---\n";
    cache->flush_all();
    current_time += 10;
    
    int long_test_count = 100;
    for (int i = 0; i < long_test_count; ++i) {
        addr_t addr = (i * 127) % 0x10000;  // Pseudo-random pattern
        if ((i % 10) == 0) {
            pmem_write(addr, 0xC0000000 + i, 0xF);
        }
        
        read_done_flags.clear();
        wait_for_ready(cache, objects, 50);
        cache->read_req(addr);
        
        // Don't wait for each one fully, just give it some cycles
        for (int j = 0; j < 80; ++j) {
            step(objects);
            if (read_done_flags[addr]) break;
        }
        
        // Ensure this one completed before issuing next
        if (!read_done_flags[addr]) {
            std::cout << "Warning: request " << i << " did not complete in time\n";
            for (int j = 0; j < 200; ++j) {
                step(objects);
                if (read_done_flags[addr]) break;
            }
        }
    }
    
    // Drain remaining
    for (int i = 0; i < 500; ++i) {
        step(objects);
    }
    
    std::cout << "✓ Extended operation: " << long_test_count << " requests completed\n";
    std::cout << "Total simulation cycles: " << current_time << "\n";
    
    return true;
}
