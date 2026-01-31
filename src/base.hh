#pragma once

#include "nlohmann/json.hpp"
#include "types.hh"
#include <iostream>
#include <string>

using json = nlohmann::ordered_json;

class SimObject {
private:
  std::string name_;

public:
  SimObject() = delete;
  SimObject(const std::string& name_in)
      : name_{name_in} {}
  virtual ~SimObject() = default;

  virtual json stats_json() const = 0;
  virtual json config_json() const = 0;

  auto
  name() const {
    return name_;
  }

  virtual auto reset_stats() -> void = 0;
  virtual void dump_stats(std::ostream& os = std::cout) const = 0;
};

class ClockedObject : public SimObject {
public:
  ClockedObject() = delete;
  ClockedObject(const std::string& name_in)
      : SimObject(name_in) {}
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
