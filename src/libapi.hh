#pragma once
// #include <cstddef>
// #include <cstdint>

// extern "C" {
// Cache API
// uintptr_t npsim_create_cache(size_t size_bytes, size_t line_bytes, size_t ways);
// void npsim_destroy_cache(uintptr_t h);
// int npsim_cache_access(uintptr_t h, uint64_t addr); // returns 1 on hit, 0 on miss
// void npsim_cache_prefetch(uintptr_t h, uint64_t addr);
// void npsim_cache_stats(uintptr_t h, uint64_t *accesses, uint64_t *hits, uint64_t *misses);

// // Branch API
// uintptr_t npsim_create_bp(unsigned index_bits);
// void npsim_destroy_bp(uintptr_t h);
// int npsim_bp_predict(uintptr_t h, uint64_t pc); // 1 taken, 0 not taken
// void npsim_bp_update(uintptr_t h, uint64_t pc, int taken);
// }
