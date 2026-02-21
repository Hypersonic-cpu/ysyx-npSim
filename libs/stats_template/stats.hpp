#pragma once

#include "nlohmann/json_fwd.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <ostream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

template<typename Derived, typename Base>
concept IsDerived = std::derived_from<Derived, Base>;

using json = nlohmann::ordered_json;

class StatsBase {
protected:
  const std::string name_;

public:
  StatsBase(const std::string& name)
      : name_{name} {}
  StatsBase() = delete;

  std::string
  name() const {
    return name_;
  }

  virtual json gen_json() const = 0;
  virtual void dump_stats(std::ostream& os = std::cout) const = 0;
  virtual void reset_stats() = 0;
};

template <typename U> class ClassifiedStats : public StatsBase {
protected:
  std::unordered_map<std::string, size_t> arr;
  const std::unordered_map<U, std::string> idToNames;
  size_t samples;

public:
  ClassifiedStats(const std::string& name,
                  const std::unordered_map<U, std::string>& nameMap)
      : StatsBase(name)
      , arr{}
      , idToNames(nameMap)
      , samples{0} {
    reset_stats();
  }

  auto
  sample(U bucket, size_t num = 1) -> void {
    samples += num;
    auto const& nm = idToNames.at(bucket);
    arr[nm] += num;
  }

  json
  gen_json() const override {
    auto brr = arr;
    brr["samples"] = samples;
    return json(brr);
  }

  void
  dump_stats(std::ostream& os) const override {
    os << std::format("{:10s} : samples {:d} ", this->name_, samples);
    for (auto const& [id, nm] : idToNames) {
      os << std::format(", {:s} {:d} ", nm, arr.at(nm));
    }
    os << std::endl;
  }

  void
  reset_stats() override {
    samples = 0;
    for (auto const& [id, nm] : idToNames) {
      arr[nm] = 0;
    }
  }

  size_t
  get_samples() const {
    return samples;
  }

  size_t
  at(U key) const {
    return arr.at(idToNames.at(key));
  }
};

template <typename T> class DistriBase : public StatsBase {
protected:
  enum MetaIdx { Overflow = 0, Underflow, Min, Max, Num_MetaIdx };
  inline static const std::vector<std::string> MetaName{
    "Overflow", "Underflow", "Min", "Max"};
  enum StatIdx { Sum = 0, Avg, Num_StatIdx };
  inline static const std::vector<std::string> StatName{"Sum", "Avg"};

  T min;
  T max;
  T delta;
  T maxidx;
  std::vector<size_t> arr;
  std::vector<size_t> meta;
  std::vector<double> stat;
  size_t samples;

public:
  DistriBase(T min, T max, T delta, const std::string& name)
      : StatsBase(name)
      , min{min}
      , max{max}
      , delta{delta}
      , maxidx{(max - min) / delta}
      , arr(maxidx, 0U)
      , meta(Num_MetaIdx, 0U)
      , stat(Num_StatIdx, 0.0)
      , samples{0} {}

  virtual void
  sample(T v, size_t n = 1) {
    samples += n;
    if (v >= max) {
      meta.at(Overflow) += n;
    } else if (v < min) {
      arr.at(Underflow) += n;
    } else {
      auto idx = (v - min) / delta;
      arr.at(idx) += n;
    }
    meta.at(Max) = std::max<T>(v, meta.at(Max));
    meta.at(Min) = std::min<T>(v, meta.at(Min));
    stat.at(Sum) += static_cast<double>(v);
    auto frac = static_cast<double>(samples - n) / samples;
    stat.at(Avg) = frac * stat.at(Avg) + (1.0 - frac) * v;
  }

  virtual void
  reset_stats() override {
    arr.assign(maxidx, 0U);
    meta.assign(Num_MetaIdx, 0U);
    stat.assign(Num_StatIdx, 0.0);
    samples = 0;
  }

public:
  json
  gen_json() const override {
    std::vector<std::string> buckets{};
    json ret = {};
    ret["samples"] = samples;
    for (auto i = 0U; i < maxidx; ++i)
      ret[std::to_string(min + delta * i)] = arr.at(i);
    for (auto i = 0U; i < StatName.size(); ++i)
      ret[StatName.at(i)] = stat.at(i);
    for (auto i = 0U; i < MetaName.size(); ++i)
      ret[MetaName.at(i)] = meta.at(i);
    return ret;
  }

  size_t
  get_samples() const {
    return samples;
  }

  double
  get_avg() const {
    return stat.at(Avg);
  }

  double
  get_sum() const {
    return stat.at(Sum);
  }

  void
  dump_stats(std::ostream& os) const override {
    os << std::format("{:10s} : samples {:10d} , avg {:f}", this->name_,
                      get_samples(), get_avg())
       << std::endl;
  }
};

template <typename T> class DistriDelta : public DistriBase<T> {
private:
  T last;

public:
  DistriDelta(T min, T max, T delta, T init, const std::string& name)
      : DistriBase<T>(min, max, delta, name)
      , last{init} {}

  void
  sample(T v, size_t n = 1) override {
    auto dpc = v - last;
    DistriBase<T>::sample(dpc, n);
    last = v;
  }

  void
  updlast(T v) {
    last = v;
  }
};

template <typename U, typename T>
  requires IsDerived<U, DistriBase<T>>
class DistriVec : public StatsBase {
protected:
  std::vector<U> cats;
  size_t sumsamples;

public:
  DistriVec(size_t n, T min, T max, T delta, T init, const std::string& name,
            const std::vector<std::string>& names)
      : StatsBase(name)
      , sumsamples{0}
      , cats{} {
    assert(n == names.size());
    for (auto i = 0U; i < n; i++) {
      auto const nm =
        names.size() ? names.at(i) : ("Cat_" + std::to_string(i));
      if constexpr (std::is_same<U, DistriDelta<T>>::value) {
        cats.emplace_back(min, max, delta, init, nm);
      } else {
        cats.emplace_back(min, max, delta, nm);
      }
    }
  }

  void
  reset_stats() override {
    for (auto& c : cats) {
      c.reset_stats();
    }
    sumsamples = 0;
  }

  void
  sample(size_t cat, T value) {
    sumsamples++;
    cats.at(cat).sample(value);
  }

  size_t
  size() const {
    return cats.size();
  }

  size_t
  samples() const {
    return sumsamples;
  }

  const U&
  operator[](size_t idx) const {
    return cats[idx];
  }

  json
  gen_json() const override {
    json ret{};
    for (auto const& c : cats) {
      ret[c.name()] = c.gen_json();
    }
    ret["samples"] = samples();
    return ret;
  }

  void
  dump_stats(std::ostream& os) const override {
    os << std::format("{} samples {:d}", this->name(), sumsamples)
       << std::endl;
    return;
    for (const auto& c : cats) {
      c.dump_stats(os);
    }
    os << std::endl;
  }
};
