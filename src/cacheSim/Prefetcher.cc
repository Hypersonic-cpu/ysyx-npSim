#include "cacheSim/Prefetcher.hh"

using namespace cacheSim;

std::optional<addr_t> NextLinePrefetcher::probe(addr_t addr, bool is_hit) {
    // Prefetch next block
    return addr + blkSize_;
}

std::optional<addr_t> StridePrefetcher::probe(addr_t addr, bool is_hit) {
    int32_t stride = static_cast<int32_t>(addr - last_addr);
    std::optional<addr_t> ret = std::nullopt;

    // Simple 1-entry stride predictor (Stream-like)
    // If stride repeats, prefetch
    if (stride != 0 && stride == last_stride) {
        // Confidence check or just prefetch
        // Prefetch next K lines? Just 1 for now.
        ret = addr + stride;
    }

    last_addr = addr;
    last_stride = stride;
    return ret;
}
