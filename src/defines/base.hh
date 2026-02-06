#pragma once

#include "nlohmann/json.hpp"
#include "stats.hpp"
#include "types.hh"
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>

using json = nlohmann::ordered_json;

class SimObject {
private:
  std::string name_;
  StatsBase* const stats_ptr_;

public:

  SimObject() = delete;
  SimObject(const std::string& name_in, StatsBase* pstats)
      : name_{name_in}
      , stats_ptr_(pstats) {}
  virtual ~SimObject() = default;

  virtual json config_json() const = 0;

  auto
  name() const {
    return name_;
  }

  virtual json
  stats_json() const {
    if (stats_ptr_)
      return stats_ptr_->gen_json();
    return {};
  }

  virtual auto
  reset_stats() -> void {
    if (stats_ptr_)
      stats_ptr_->reset_stats();
  }

  virtual void
  dump_stats(std::ostream& os = std::cout) const {
    if (stats_ptr_)
      stats_ptr_->dump_stats(os);
  }
};

class ClockedObject : public SimObject {
public:
  ClockedObject() = delete;
  ClockedObject(const std::string& name_in, StatsBase* pstats)
      : SimObject(name_in, pstats) {}
  virtual ~ClockedObject() = default;

  virtual tick_t next_update() const = 0;
  virtual void update_impl() = 0;

  void
  do_update() {
    if (next_update() <= curr_tick()) {
      update_impl();
    }
  }
};
