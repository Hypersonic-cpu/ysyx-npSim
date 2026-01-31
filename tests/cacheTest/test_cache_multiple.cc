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

// Global simulation time
static tick_t current_time = 0;
tick_t curr_tick() noexcept {
    return current_time;
}

// Response tracking for two caches
struct ResponseLog {
    tick_t time;
    uint16_t cache_id;  // 0=ICache, 1=DCache
    addr_t addr;
    bool is_write;
    word_t data;
};

static std::vector<ResponseLog> response_log;
static std::map<addr_t, word_t> icache_read_data;
static std::map<addr_t, word_t> dcache_read_data;
static std::map<addr_t, bool> icache_done;
static std::map<addr_t, bool> dcache_write_done;

void icache_read_callback(addr_t addr, word_t ret) {
    icache_read_data[addr] = ret;
    icache_done[addr] = true;
    response_log.push_back({current_time, 0, addr, false, ret});
    std::cout << "[T=" << std::setw(5) << current_time << "] ICache Read Response: Addr=0x" 
              << std::hex << addr << " Data=0x" << ret << std::dec << std::endl;
}

void dcache_read_callback(addr_t addr, word_t ret) {
    dcache_read_data[addr] = ret;
    response_log.push_back({current_time, 1, addr, false, ret});
    std::cout << "[T=" << std::setw(5) << current_time << "] DCache Read Response: Addr=0x" 
              << std::hex << addr << " Data=0x" << ret << std::dec << std::endl;
}

void dcache_write_callback(addr_t addr) {
    dcache_write_done[addr] = true;
    response_log.push_back({current_time, 1, addr, true, 0});
    std::cout << "[T=" << std::setw(5) << current_time << "] DCache Write Response: Addr=0x" 
              << std::hex << addr << std::dec << std::endl;
}

void step(std::vector<ClockedObject*>& objects, int cycles = 1) {
    for (int i = 0; i < cycles; ++i) {
        current_time++;
        for (auto* obj : objects) {
            obj->do_update();
        }
    }
}

bool wait_for_ready(cacheSim::CacheBase* cache, std::vector<ClockedObject*>& objects, 
                    bool wait_read = true, int max_cycles = 100) {
    for (int i = 0; i < max_cycles; ++i) {
        auto [r_ready, w_ready] = cache->is_ready();
        if (wait_read && r_ready) return true;
        if (!wait_read && w_ready) return true;
        step(objects);
    }
    return false;
}

void analyze_arbitration() {
    std::cout << "\n=== Arbitration Analysis ===" << std::endl;
    std::cout << "Total responses: " << response_log.size() << std::endl;
    
    int icache_responses = 0;
    int dcache_read_responses = 0;
    int dcache_write_responses = 0;
    
    for (const auto& log : response_log) {
        if (log.cache_id == 0) {
            icache_responses++;
        } else {
            if (log.is_write) dcache_write_responses++;
            else dcache_read_responses++;
        }
    }
    
    std::cout << "ICache responses: " << icache_responses << std::endl;
    std::cout << "DCache read responses: " << dcache_read_responses << std::endl;
    std::cout << "DCache write responses: " << dcache_write_responses << std::endl;
    
    // Check for interleaving - both caches getting serviced
    std::cout << "\nResponse sequence (first 30):" << std::endl;
    for (size_t i = 0; i < std::min(size_t(30), response_log.size()); ++i) {
        const auto& log = response_log[i];
        std::cout << "  [" << log.time << "] " 
                  << (log.cache_id == 0 ? "ICache" : "DCache")
                  << " " << (log.is_write ? "Write" : "Read") << std::endl;
    }
}

int main() {
    std::cout << "==============================================\n";
    std::cout << "     Multiple Cache Arbiter Test\n";
    std::cout << "     ICache (ID=0, ReadOnly, Pipelined)\n";
    std::cout << "     DCache (ID=1, R/W, NoCache)\n";
    std::cout << "==============================================\n\n";
    
    // Configuration
    const size_t CACHE_SIZE = 4096;
    const size_t LINE_SIZE = 64;
    const size_t ASSOC = 4;
    const size_t PIPE_DEPTH = 3;
    const tint_t MEM_LATENCY = 50;
    const tint_t BURST_LATENCY = 1;
    
    // Create ICache (pipelined, readonly, ID=0)
    auto icache = std::make_unique<cacheSim::PipeCache>(
        "ICache", PIPE_DEPTH, CACHE_SIZE, LINE_SIZE, ASSOC, nullptr, 0
    );
    icache->set_resp_handlers(icache_read_callback, nullptr);
    
    // Create DCache (NoCache, read-write, ID=1)
    auto dcache = std::make_unique<cacheSim::NoCache>("DCache", 1);
    dcache->set_resp_handlers(dcache_read_callback, dcache_write_callback);
    
    // Create RAM arbiter with both caches
    // Order matters: ICache (ID=0), DCache (ID=1)
    // Higher ID = higher priority (DCache has priority over ICache)
    std::vector<cacheSim::CacheBase*> caches = { icache.get(), dcache.get() };
    auto ram = std::make_unique<memSim::RAMArbiter>(
        "RAM", MEM_LATENCY, BURST_LATENCY, caches
    );
    
    icache->set_mem_port(ram.get());
    dcache->set_mem_port(ram.get());
    
    std::vector<ClockedObject*> objects = { icache.get(), dcache.get(), ram.get() };
    
    // Setup memory
    std::cout << "Setting up memory..." << std::endl;
    for (int i = 0; i < 50; ++i) {
        pmem_write(0x10000 + i * 4, 0xA0000000 + i, 0xF);  // ICache region
        pmem_write(0x20000 + i * 4, 0xB0000000 + i, 0xF);  // DCache region
    }
    std::cout << "Memory setup complete\n" << std::endl;
    
    // TEST 1: Simultaneous access - verify arbiter handles conflicts
    std::cout << "========================================" << std::endl;
    std::cout << "TEST 1: Simultaneous Access" << std::endl;
    std::cout << "Both caches request at same time" << std::endl;
    std::cout << "Expected: DCache (ID=1) gets priority" << std::endl;
    std::cout << "========================================\n" << std::endl;
    
    response_log.clear();
    current_time = 0;
    
    // Issue requests to both caches in same cycle
    wait_for_ready(icache.get(), objects, true);
    wait_for_ready(dcache.get(), objects, true);
    
    std::cout << "Issuing simultaneous requests at cycle " << current_time << std::endl;
    icache->read_req(0x10000);
    dcache->read_req(0x20000);
    
    // Let both requests complete
    for (int i = 0; i < 300; ++i) {
        step(objects);
        if (icache_done[0x10000] && dcache_read_data.count(0x20000)) break;
    }
    
    std::cout << "\nBoth requests completed at cycle " << current_time << std::endl;
    
    // Check which one completed first (DCache should have priority)
    tick_t icache_time = 0, dcache_time = 0;
    for (const auto& log : response_log) {
        if (log.cache_id == 0 && log.addr == 0x10000) icache_time = log.time;
        if (log.cache_id == 1 && log.addr == 0x20000) dcache_time = log.time;
    }
    
    std::cout << "ICache response time: " << icache_time << std::endl;
    std::cout << "DCache response time: " << dcache_time << std::endl;
    
    bool priority_correct = true;
    if (dcache_time < icache_time) {
        std::cout << "✓ PASS: DCache (higher priority) completed first" << std::endl;
    } else {
        std::cout << "✗ FAIL: ICache completed first (expected DCache)" << std::endl;
        priority_correct = false;
    }
    
    // TEST 2: Read priority over write (both from DCache)
    std::cout << "\n========================================" << std::endl;
    std::cout << "TEST 2: Read vs Write Priority" << std::endl;
    std::cout << "DCache read and write requests" << std::endl;
    std::cout << "Expected: Read completes before write" << std::endl;
    std::cout << "========================================\n" << std::endl;
    
    response_log.clear();
    dcache_read_data.clear();
    dcache_write_done.clear();
    current_time += 10;
    
    // Wait for DCache to be ready
    wait_for_ready(dcache.get(), objects, true);
    
    std::cout << "Issuing DCache write at cycle " << current_time << std::endl;
    dcache->write_req(0x20100, 0xDEADBEEF, 0xF);
    
    // Wait a bit for write to start
    step(objects, 5);
    
    std::cout << "Issuing DCache read at cycle " << current_time << std::endl;
    wait_for_ready(dcache.get(), objects, true);
    dcache->read_req(0x20200);
    
    // Let both complete
    for (int i = 0; i < 300; ++i) {
        step(objects);
        if (dcache_write_done[0x20100] && dcache_read_data.count(0x20200)) break;
    }
    
    std::cout << "\nBoth requests completed at cycle " << current_time << std::endl;
    
    tick_t read_time = 0, write_time = 0;
    for (const auto& log : response_log) {
        if (log.cache_id == 1 && log.addr == 0x20200 && !log.is_write) read_time = log.time;
        if (log.cache_id == 1 && log.addr == 0x20100 && log.is_write) write_time = log.time;
    }
    
    std::cout << "Read response time: " << read_time << std::endl;
    std::cout << "Write response time: " << write_time << std::endl;
    
    bool read_priority_correct = true;
    if (read_time < write_time) {
        std::cout << "✓ PASS: Read completed before write" << std::endl;
    } else {
        std::cout << "✗ FAIL: Write completed first (expected read)" << std::endl;
        read_priority_correct = false;
    }
    
    // TEST 3: Sustained concurrent access
    std::cout << "\n========================================" << std::endl;
    std::cout << "TEST 3: Sustained Concurrent Access" << std::endl;
    std::cout << "Alternate between ICache and DCache" << std::endl;
    std::cout << "========================================\n" << std::endl;
    
    response_log.clear();
    icache_done.clear();
    dcache_read_data.clear();
    current_time += 10;
    
    std::cout << "Starting interleaved access pattern..." << std::endl;
    
    int num_requests = 20;
    for (int i = 0; i < num_requests; ++i) {
        if (i % 2 == 0) {
            // ICache request
            wait_for_ready(icache.get(), objects, true);
            addr_t addr = 0x10000 + i * 4;
            icache->read_req(addr);
            std::cout << "[Req] ICache 0x" << std::hex << addr << std::dec << std::endl;
        } else {
            // DCache request
            wait_for_ready(dcache.get(), objects, true);
            addr_t addr = 0x20000 + i * 4;
            dcache->read_req(addr);
            std::cout << "[Req] DCache 0x" << std::hex << addr << std::dec << std::endl;
        }
        
        // Give a few cycles between requests
        step(objects, 3);
    }
    
    // Wait for all to complete
    for (int i = 0; i < 1000; ++i) {
        step(objects);
        if (response_log.size() >= num_requests) break;
    }
    
    std::cout << "\nCompleted " << response_log.size() << "/" << num_requests 
              << " requests" << std::endl;
    
    analyze_arbitration();
    
    bool concurrent_ok = (response_log.size() == num_requests);
    if (concurrent_ok) {
        std::cout << "\n✓ PASS: All concurrent requests completed" << std::endl;
    } else {
        std::cout << "\n✗ FAIL: Some requests did not complete" << std::endl;
    }
    
    // TEST 4: Heavy contention
    std::cout << "\n========================================" << std::endl;
    std::cout << "TEST 4: Heavy Contention" << std::endl;
    std::cout << "Multiple requests from both caches" << std::endl;
    std::cout << "========================================\n" << std::endl;
    
    response_log.clear();
    icache_done.clear();
    dcache_read_data.clear();
    dcache_write_done.clear();
    current_time += 10;
    
    // Warm up ICache
    std::cout << "Warming up ICache..." << std::endl;
    wait_for_ready(icache.get(), objects, true);
    icache->read_req(0x10000);
    for (int i = 0; i < 200; ++i) {
        step(objects);
        if (icache_done[0x10000]) break;
    }
    
    response_log.clear();
    std::cout << "Starting heavy contention test..." << std::endl;
    
    // Issue many requests rapidly
    std::vector<addr_t> icache_addrs = {0x10000, 0x10004, 0x10008, 0x1000C, 0x10010};
    std::vector<addr_t> dcache_addrs = {0x20000, 0x20100, 0x20200};
    
    // ICache requests (should all hit)
    for (auto addr : icache_addrs) {
        wait_for_ready(icache.get(), objects, true, 20);
        icache->read_req(addr);
        step(objects, 1);
    }
    
    // DCache requests (all miss)
    for (auto addr : dcache_addrs) {
        wait_for_ready(dcache.get(), objects, true, 20);
        dcache->read_req(addr);
        step(objects, 1);
    }
    
    // Wait for all
    for (int i = 0; i < 500; ++i) {
        step(objects);
    }
    
    std::cout << "\nReceived " << response_log.size() << " responses" << std::endl;
    
    int expected = icache_addrs.size() + dcache_addrs.size();
    bool contention_ok = (response_log.size() >= expected);
    if (contention_ok) {
        std::cout << "✓ PASS: All requests under contention completed" << std::endl;
    } else {
        std::cout << "✗ FAIL: Some requests lost under contention" << std::endl;
    }
    
    // Final summary
    std::cout << "\n==============================================\n";
    bool all_pass = priority_correct && read_priority_correct && concurrent_ok && contention_ok;
    if (all_pass) {
        std::cout << "     ✓ ALL ARBITER TESTS PASSED\n";
    } else {
        std::cout << "     ✗ SOME ARBITER TESTS FAILED\n";
    }
    std::cout << "==============================================\n";
    
    return all_pass ? 0 : 1;
}
