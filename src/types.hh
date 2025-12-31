#pragma once
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>

using addr_t = uint32_t;
using word_t = uint32_t;
using tick_t = size_t;
using tint_t = uint32_t; // time interval

constexpr addr_t WordShift{2};

extern tick_t curr_tick();

extern tint_t pmem_read(addr_t addr, addr_t* ret, bool bfirst);
extern tint_t pmem_write(addr_t addr, word_t data, unsigned char mask, bool bfirst);

inline bool
isDevice(addr_t a) {
  return (a >= 0x10000000U && a < 0x80000000U);
}

inline bool
isPowerOf2(addr_t x) {
  return (x != 0) && ((x & (x - 1)) == 0);
}

inline size_t
floorLog2(size_t x) {
  assert(x > 0);
  return static_cast<size_t>(std::bit_width(x) - 1);
}

#define CACHE false // true
#define SIMPRINTFN(Cat, Fmt, ...) \
  do { \
    if (Cat) \
      printf("[ @ %08lx ] " Fmt "\n", \
        curr_tick(), ##__VA_ARGS__); \
  } while (0)
