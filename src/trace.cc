#include "trace.hh"
#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>

namespace trace {

// bool
// TraceReader::isxz(const std::string& filename) {
//   auto dot_pos = filename.find_last_of(".");
//   auto ret =
//     dot_pos < filename.size() && filename.substr(dot_pos + 1) == "xz";
//   return ret;
// }

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

} // namespace trace
