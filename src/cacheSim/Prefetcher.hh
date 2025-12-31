#pragma once
#include "../types.hh"
#include <optional>
#include <cstddef>

namespace cacheSim {

class Prefetcher {
public:
    Prefetcher() = default;
    // called on every access; may record history; return optional address to prefetch
    std::optional<addr_t> probe(addr_t addr, uint64_t stamp);
    // called when cache wants to perform prefetch; may return addr to prefetch or empty
    std::optional<addr_t> prefetch(addr_t /*addr*/, uint64_t /*stamp*/){ return std::nullopt; }
};

} // namespace cacheSim
