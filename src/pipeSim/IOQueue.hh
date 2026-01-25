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
  const size_t size;
  std::list<BufEntry> queue;

public:
  IOQueue(size_t size)
      : size(size)
      , queue{} {}

  void
  enqueue(tick_t finish, addr_t addr) {
    assert(!is_full());
    tick_t t = std::max(finish, next_avaiable());
    queue.emplace_back(t, addr);
  }

  tick_t
  next_avaiable() const {
    return queue.empty() ? 0U : queue.front().time;
  }

  bool
  is_full() const {
    return queue.size() == size;
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
    for (const auto &entry : queue) {
      if (entry.addr == addr) {
        return true;
      }
    }
    return false;
  }
};
