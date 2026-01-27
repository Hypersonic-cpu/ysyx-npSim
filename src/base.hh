#pragma once

#include <string>
#include <unordered_map>
#include <iostream>
#include "nlohmann/json.hpp"

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
