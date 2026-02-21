#pragma once
#include <nlohmann/json.hpp>
#include <cstddef>

namespace area {

using json = nlohmann::ordered_json;

/** Estimate flip-flop area at NanGate 45nm.
    A DFF is roughly 5 um² per bit at 45nm. */
inline double
dff_area_um2(size_t bits) {
  return bits * 5.0;
}

/** Build an area JSON node for a pure-combinational component
    (no SRAM, just known timing area from STA). */
inline json
comb_only(double timing_area_um2) {
  json j;
  j["comb_percent"] = 0.0;
  j["timing_area"] = timing_area_um2;
  j["cacti_objs"] = json::array();
  return j;
}

/** Build a CACTI cache object descriptor. */
inline json
cacti_cache(const std::string& label, size_t size_bytes,
            size_t block_bytes, size_t assoc) {
  json j;
  j["label"] = label;
  j["type"] = "cache";
  j["size"] = size_bytes;
  j["block_size"] = block_bytes;
  j["assoc"] = assoc;
  return j;
}

/** Build a CACTI RAM object descriptor (for small tables like BPU). */
inline json
cacti_ram(const std::string& label, size_t size_bytes,
          size_t word_bytes) {
  json j;
  j["label"] = label;
  j["type"] = "ram";
  j["size"] = size_bytes;
  j["word_size"] = word_bytes;
  return j;
}

} // namespace area
