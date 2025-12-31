#include "Prefetcher.hh"

using namespace cacheSim;

std::optional<addr_t> Prefetcher::probe(addr_t /*addr*/, uint64_t /*stamp*/){
    return std::nullopt; // default: no prefetch
}
