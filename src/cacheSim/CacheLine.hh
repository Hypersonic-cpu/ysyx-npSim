#pragma once
#include "../types.hh"
#include <cassert>
#include <vector>

namespace cacheSim {

class CacheLine {
private:
  addr_t tag;
  bool valid;
  const size_t lineSize_;
  std::vector<word_t> data;

public:
  CacheLine() = delete;
  CacheLine(size_t line_size)
      : lineSize_(line_size)
      , data(line_size >> WordShift, 0) {
    invalidate();
  }

  void
  invalidate() {
    this->tag = 0;
    this->stamp = 0;
    this->valid = false;
  }

  tick_t stamp;

  bool
  isValid() const {
    return this->valid;
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

  template <typename T>
  T*
  getRawData() {
    return reinterpret_cast<T*>(this->data.data());
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
