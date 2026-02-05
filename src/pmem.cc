#include "pmem.hh"
#include <map>

// Simple sparse memory model for testing
static std::map<addr_t, uint8_t> memory;

tick_t
pmem_read(addr_t addr, word_t* data) {
  // word_t val = 0;
  // for (int i = 0; i < 4; ++i) {
  //     uint8_t byte = 0;
  //     if (memory.count(addr + i)) {
  //         byte = memory[addr + i];
  //     }
  //     val |= (static_cast<word_t>(byte) << (i * 8));
  // }
  // *data = val;
  return 1;
}

tick_t
pmem_write(addr_t addr, word_t data, uint8_t mask) {
  // for (int i = 0; i < 4; ++i) {
  //     if ((mask >> i) & 1) {
  //         uint8_t byte = (data >> (i * 8)) & 0xFF;
  //         memory[addr + i] = byte;
  //     }
  // }
  return 1;
}
