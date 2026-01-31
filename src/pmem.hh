#pragma once

#include "types.hh"

// Read from physical memory (4 bytes)
tick_t pmem_read(addr_t addr, word_t* data);

// Write to physical memory (4 bytes with mask)
tick_t pmem_write(addr_t addr, word_t data, uint8_t mask);
