#pragma once

/**
 * npc/ should re-define these classes and SHOULD NOT
 * include this header.
 */

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>

using addr_t = uint32_t;
using word_t = uint32_t;
using tick_t = size_t;
using tint_t = uint32_t; // time interval

constexpr addr_t WordShift{2};
constexpr tick_t InfTime{std::numeric_limits<tick_t>::max()};

extern tick_t curr_tick() noexcept;
