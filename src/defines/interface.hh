#pragma once

#include "types.hh"
#include <functional>
#include <memory>
#include <vector>

enum class Direction { Req, Resp };
enum class MemRWOpt { Read, Write };

struct MemTrans {
  Direction dir;
  MemRWOpt mop;
  addr_t addr;
  uint16_t id;
  uint16_t bst_len;
  // Write req required, Read resp used
  std::vector<word_t> data;
  std::vector<uint8_t> strb;
  // Set by device
  tint_t lat;
};
using MemTransPtr = std::unique_ptr<MemTrans>;

struct CpuTrans {
  addr_t addr;
  word_t data;
  uint16_t id;
  MemRWOpt mop;
};

struct AckTrans {
  uint16_t id;
  MemRWOpt mop;
};

using CpuSideMRespReceiver = std::function<void(CpuTrans)>;
using CpuSideAckReceiver = std::function<void(AckTrans)>;

using CacheMRespHandler =
  std::function<void(addr_t, const std::vector<word_t>&)>;
