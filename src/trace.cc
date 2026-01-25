#include "trace.hh"
#include <iostream>

namespace trace {

TraceReader::TraceReader(const char* filename) {
  file_.open(filename, std::ios::binary);
  if (!file_) {
    std::cerr << "Failed to open trace file: " << filename << "\n";
  }
}

bool
TraceReader::next(TraceInst& inst) {
  return file_.read(reinterpret_cast<char*>(&inst), sizeof(TraceInst))
    .good();
}

} // namespace trace
