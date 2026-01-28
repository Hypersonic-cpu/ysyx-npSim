#pragma once

#include "types.hh"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <list>

class IOQueue {
public:
  struct BufEntry {
    tick_t time;
    addr_t addr;
  };

private:
  const size_t entries;
  std::list<BufEntry> queue;

public:
  IOQueue(size_t size)
      : entries(size)
      , queue{} {}

  size_t
  capacity() const {
    return entries;
  }

  void
  enqueue(tick_t finish, addr_t addr) {
    assert(!is_full());
    tick_t t = std::max(finish, next_poptime());
    queue.emplace_back(t, addr);
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

  void
  dequeue() {
    assert(!queue.empty());
    queue.pop_front();
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

  bool
  size() const {
    return queue.size();
  }
};
