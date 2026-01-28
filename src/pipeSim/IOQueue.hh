#pragma once

#include "types.hh"
#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <list>

namespace pipeSim {
struct IOEntryBase {
  tick_t time;
  addr_t addr;
};

template <std::derived_from<IOEntryBase> T>
class IOQueue {
public:
private:
  const size_t entries;
  std::list<T> queue;

public:
  IOQueue(size_t size)
      : entries(size)
      , queue{} {}

  size_t
  capacity() const {
    return entries;
  }

  void
  enqueue(T elem) {
    assert(!is_full());
    tick_t t = std::max(elem.time, next_poptime());
    elem.time = t;
    queue.emplace_back(std::move(elem));
  }

  tick_t
  next_poptime() const {
    return queue.empty() ? 0U : queue.front().time;
  }

  // tick_t
  // last_poptime() const {
  //   return queue.empty() ? 0U : queue.back().time;
  // }

  bool
  is_full() const {
    return queue.size() == entries;
  }

  bool
  is_empty() const {
    return queue.empty();
  }

  T
  dequeue() {
    assert(!queue.empty());
    T elem = std::move(queue.front());
    queue.pop_front();
    return std::move(elem);
  }

  void
  auto_dequeue(tick_t curr_time) {
    while (!queue.empty() && queue.front().time <= curr_time) {
      queue.pop_front();
    }
  }

  bool
  contains(addr_t addr) const {
    for (const auto& entry : queue) {
      if (entry.addr == addr) {
        return true;
      }
    }
    return false;
  }

  auto
  size() const {
    return queue.size();
  }
};
} // namespace pipeSim
