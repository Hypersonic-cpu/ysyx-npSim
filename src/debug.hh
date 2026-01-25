#pragma once
#include <cstdint>
#include <string>
#include <cstdio>
#include <sstream>

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
    Mem = LDQueue | STQueue,
    All = UINT64_MAX
};

extern uint64_t enabled_flags;

inline void set_flags(const std::string& flag_str) {
    std::stringstream ss(flag_str);
    std::string segment;
    while (std::getline(ss, segment, ',')) {
        if (segment == "Cache") enabled_flags |= Cache;
        else if (segment == "BranchPred") enabled_flags |= BranchPred;
        else if (segment == "Pipeline") enabled_flags |= Pipeline;
        else if (segment == "IFQueue") enabled_flags |= IFQueue;
        else if (segment == "LDQueue") enabled_flags |= LDQueue;
        else if (segment == "STQueue") enabled_flags |= STQueue;
        else if (segment == "Mem") enabled_flags |= Mem;
        else if (segment == "Main") enabled_flags |= Main;
        else if (segment == "All") enabled_flags |= All;
    }
}

#define DPRINTF(flag, fmt, ...) \
    do { \
        if (debug::enabled_flags & debug::flag) { \
            fprintf(stderr, "[%s] " fmt "\n", #flag, ##__VA_ARGS__); \
        } \
    } while(0)

}
