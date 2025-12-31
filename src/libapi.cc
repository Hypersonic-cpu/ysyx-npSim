
// #include "branchSim/BranchPredictor.hh"
// #include "cacheSim/CacheSimulator.hh"
// #include "libapi.hh"
// #include <memory>
// #include <unordered_map>

// using namespace cacheSim;
// using namespace branchSim;

// static std::unordered_map<uintptr_t, std::unique_ptr<CacheSimulator>>
//   g_caches;
// static std::unordered_map<uintptr_t, std::unique_ptr<BranchPredictor>> g_bps;
// static uintptr_t g_next = 1;

// // cycle source provided by simulator environment (DPI-C/Verilator)
// extern "C" uint64_t npc_tick();

// extern "C" {

// uintptr_t
// npsim_create_cache(size_t size_bytes, size_t line_bytes, size_t ways) {
//   auto p = std::make_unique<CacheSimulator>(size_bytes, line_bytes, ways);
//   uintptr_t h = g_next++;
//   g_caches[h] = std::move(p);
//   return h;
// }

// void
// npsim_destroy_cache(uintptr_t h) {
//   g_caches.erase(h);
// }

// int
// npsim_cache_access(uintptr_t h, uint64_t addr) {
//   auto it = g_caches.find(h);
//   if (it == g_caches.end())
//     return 0;
//   return it->second->access((addr_t)addr, npc_tick()) ? 1 : 0;
// }

// void
// npsim_cache_prefetch(uintptr_t h, uint64_t addr) {
//   auto it = g_caches.find(h);
//   if (it != g_caches.end())
//     it->second->handle_prefetch((addr_t)addr, npc_tick());
// }

// void
// npsim_cache_stats(uintptr_t h, uint64_t* accesses, uint64_t* hits,
//                   uint64_t* misses) {
//   auto it = g_caches.find(h);
//   if (it == g_caches.end()) {
//     if (accesses)
//       *accesses = 0;
//     if (hits)
//       *hits = 0;
//     if (misses)
//       *misses = 0;
//     return;
//   }
//   auto s = it->second->stats();
//   if (accesses)
//     *accesses = s.accesses;
//   if (hits)
//     *hits = s.hits;
//   if (misses)
//     *misses = s.misses;
// }

// uintptr_t
// npsim_create_bp(unsigned index_bits) {
//   auto p = std::make_unique<BimodalPredictor>(index_bits);
//   uintptr_t h = g_next++;
//   g_bps[h] = std::move(p);
//   return h;
// }
// void
// npsim_destroy_bp(uintptr_t h) {
//   g_bps.erase(h);
// }
// int
// npsim_bp_predict(uintptr_t h, uint64_t pc) {
//   auto it = g_bps.find(h);
//   if (it == g_bps.end())
//     return 0;
//   return it->second->predict((addr_t)pc) ? 1 : 0;
// }
// void
// npsim_bp_update(uintptr_t h, uint64_t pc, int taken) {
//   auto it = g_bps.find(h);
//   if (it != g_bps.end())
//     it->second->update((addr_t)pc, taken != 0);
// }

// } // extern "C"
