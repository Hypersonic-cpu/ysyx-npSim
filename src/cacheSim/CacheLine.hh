#pragma once
#include "../types.hh"
#include <cassert>
#include <iostream>
#include <vector>

namespace cacheSim {

class CacheLine {
private:
  addr_t tag;
  bool valid;
  bool dirty;
  [[maybe_unused]]
  const size_t lineSize_;
  std::vector<word_t> data;

public:
  CacheLine() = delete;
  CacheLine(size_t line_size)
      : lineSize_(line_size)
      , tag(0)
      , valid(false)
      , dirty(false)
      , data(line_size >> WordShift, 0)
      , stamp(0)
      , is_prefetched(false) {}

  void
  invalidate() {
    // SIMPRINTFN(CACHE, "Invalidating tag %x", tag);
    this->tag = 0;
    this->stamp = 0;
    this->valid = false;
  }

  void
  activate() {
    this->valid = true;
    this->dirty = false;
    this->stamp = curr_tick();
  }

  tick_t stamp;
  // tick_t ready;
  bool is_prefetched;

  bool
  isValid() const {
    return this->valid;
  }

  bool
  isDirty() const {
    return this->dirty;
  }

  addr_t
  getTag() const {
    return this->tag;
  }

  void
  setTag(addr_t new_tag) {
    this->tag = new_tag;
  }

  void
  setValid() {
    this->valid = true;
  }

  void
  setDirty() {
    this->dirty = true;
  }

  template <typename T>
  T*
  getRawData() {
    return reinterpret_cast<T*>(this->data.data());
  }

  void
  setVecData(const std::vector<word_t>& in) {
#if ACTIVE_MODE
    // Do not do anyting for timing-only trace sim.
#else
    assert(in.size() == this->data.size());
    std::copy(in.begin(), in.end(), data.begin());
#endif
  }

  word_t
  atAligned(size_t off) const {
    return this->data.at(off >> WordShift);
  }

  word_t&
  atAligned(size_t off) {
    return this->data.at(off >> WordShift);
  }
};
} // namespace cacheSim
