#include "trace.hh"
#include <cassert>
#include <cstdio>
#include <iostream>
#include <print>
#include <string>

namespace trace {

TraceReader::TraceReader(const std::string& filename)
    : file_(popen(std::string("zstdcat -f " + filename).c_str(), "r")) {
  assert(file_ && "Failed to open trace\n");
}

TraceReader::~TraceReader() {
  if (file_)
    pclose(file_);
}

bool
TraceReader::next(TraceInst& inst) {
  if (!file_)
    return false;
  return fread(&inst, sizeof(TraceInst), 1, file_);
}

void
TraceSanitizer::dump() const {
  std::println("TraceSanitizer:");
  std::println("  Total:          {:>10}", total);
  std::println("  Loads:          {:>10} ({:.2f}%)",
               loads, 100.0 * loads / total);
  std::println("  Stores:         {:>10} ({:.2f}%)",
               stores, 100.0 * stores / total);
  std::println("  Branches:       {:>10} ({:.2f}%)",
               branches, 100.0 * branches / total);
  std::println("    Taken:        {:>10} ({:.2f}%)",
               br_taken, 100.0 * br_taken / total);
  std::println("    Not Taken:    {:>10} ({:.2f}%)",
               br_not_taken, 100.0 * br_not_taken / total);
  std::println("  ALU (non-mem/br):{:>9} ({:.2f}%)",
               alu, 100.0 * alu / total);
  std::println("  Has dst reg:    {:>10} ({:.2f}%)",
               has_dst, 100.0 * has_dst / total);
  std::println("  Has src1:       {:>10} ({:.2f}%)",
               has_src1, 100.0 * has_src1 / total);
  std::println("  Has src2:       {:>10} ({:.2f}%)",
               has_src2, 100.0 * has_src2 / total);
  std::println("  RV-M ops:       {:>10} ({:.2f}%)", 
               m_ext_ops, 100.0 * m_ext_ops / total);
  std::println("  Sys ops:        {:>10}", sys_ops);
  if (br_taken_no_target > 0)
    std::println("  !! BR TAKEN NO TARGET: {:>4}", br_taken_no_target);
}

} // namespace trace
