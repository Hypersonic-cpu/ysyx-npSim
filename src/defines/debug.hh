#pragma once
#include <cstdint>
#include <sstream>
#include <string>

namespace debug {

enum Flag : uint64_t {
  None = 0,
  Main = 1U << 0,
  Cache = 1U << 1,
  BranchPred = 1U << 2,
  Pipeline = 1U << 3,
  IFQueue = 1U << 4,
  LDQueue = 1U << 5,
  STQueue = 1U << 6,
  LSUnit = LDQueue | STQueue,
  Mem = 1U << 7,
  Clock = 1ULL << 62,
  Event = 1ULL << 63,
  All = UINT64_MAX
};

extern uint64_t enabled_flags;

inline void
set_flags(const std::string& flag_str) {
  std::stringstream ss(flag_str);
  std::string segment;
  while (std::getline(ss, segment, ',')) {
    if (segment == "Cache")
      enabled_flags |= Cache;
    else if (segment == "BranchPred")
      enabled_flags |= BranchPred;
    else if (segment == "Pipeline")
      enabled_flags |= Pipeline;
    else if (segment == "IFQueue")
      enabled_flags |= IFQueue;
    else if (segment == "LDQueue")
      enabled_flags |= LDQueue;
    else if (segment == "STQueue")
      enabled_flags |= STQueue;
    else if (segment == "LSUnit")
      enabled_flags |= LSUnit;
    else if (segment == "Mem")
      enabled_flags |= Mem;
    else if (segment == "Main")
      enabled_flags |= Main;
    else if (segment == "Clock")
      enabled_flags |= Clock;
    else if (segment == "Event")
      enabled_flags |= Event;
    else if (segment == "All")
      enabled_flags |= All;
  }
}

#ifdef NDEBUG
#define DPRINTF(flag, fmt, ...)                                             \
  do {                                                                      \
  } while (0)
#define DPRINTFS(flag, fmt, ...)                                            \
  do {                                                                      \
  } while (0)
#else
#define DPRINTFI(flag, fmt, ...)                                             \
  do {                                                                      \
    if (debug::enabled_flags & debug::flag) [[unlikely]] {                  \
      fprintf(stderr, "%lu: [%s] " fmt, curr_tick(),                   \
              this->name().c_str(), ##__VA_ARGS__);                         \
    }                                                                       \
  } while (0)

#define DPRINTF(flag, fmt, ...)                                             \
  do {                                                                      \
    if (debug::enabled_flags & debug::flag) [[unlikely]] {                  \
      fprintf(stderr, "%lu: [%s] " fmt "\n", curr_tick(),                   \
              this->name().c_str(), ##__VA_ARGS__);                         \
    }                                                                       \
  } while (0)

#define DPRINTFS(flag, fmt, ...)                                            \
  do {                                                                      \
    if (debug::enabled_flags & debug::flag) {                               \
      [[unlikely]] fprintf(stderr, "%lu: [%s] " fmt "\n", curr_tick(),      \
                           #flag, ##__VA_ARGS__);                           \
    }                                                                       \
  } while (0)
#endif
} // namespace debug

#define ANSI_FG_BLACK "\33[1;30m"
#define ANSI_FG_RED "\33[1;31m"
#define ANSI_FG_GREEN "\33[1;32m"
#define ANSI_FG_YELLOW "\33[1;33m"
#define ANSI_FG_BLUE "\33[1;34m"
#define ANSI_FG_MAGENTA "\33[1;35m"
#define ANSI_FG_CYAN "\33[1;36m"
#define ANSI_FG_WHITE "\33[1;37m"
#define ANSI_BG_BLACK "\33[1;40m"
#define ANSI_BG_RED "\33[1;41m"
#define ANSI_BG_GREEN "\33[1;42m"
#define ANSI_BG_YELLOW "\33[1;43m"
#define ANSI_BG_BLUE "\33[1;44m"
#define ANSI_BG_MAGENTA "\33[1;45m"
#define ANSI_BG_CYAN "\33[1;46m"
#define ANSI_BG_WHITE "\33[1;47m"
#define ANSI_ALL_NONE "\33[0m"
