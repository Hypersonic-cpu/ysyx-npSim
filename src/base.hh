#pragma once

#include <string>
#include <unordered_map>

class SimObject {
private:
  std::string name_;

public:
  SimObject() = default;
  SimObject(const std::string& name_in)
      : name_{name_in} {}
  virtual ~SimObject() = default;

  virtual auto
  stats_map() const -> std::unordered_map<std::string, double> = 0;
  virtual auto
  config_map() const -> std::unordered_map<std::string, size_t> = 0;

  auto
  name() const {
    return name_;
  }

  virtual auto reset_stats() -> void = 0;
};
