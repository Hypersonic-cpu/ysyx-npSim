#pragma once
#include <nlohmann/json.hpp>
#include <cstddef>

namespace area {

using json = nlohmann::ordered_json;

/** Build an area JSON node for a component whose area comes entirely from
    STA (known_area_um2) and has no SRAM macros or explicit DFF counts.
    timing_bits defaults to 0 and known_area is process-calibrated from STA. */
inline json
area_json(double known_um2) {
  json j;
  j["known_area"] = known_um2;
  j["comb_percent"] = 0.0;
  j["timing_bits"] = 0;
  j["cacti_objs"] = json::array();
  return j;
}

inline json
area_json(double known_um2, size_t dff_bits, double comb_percent) {
  json j;
  j["known_area"] = known_um2;
  j["comb_percent"] = comb_percent;
  j["timing_bits"] = dff_bits;
  j["cacti_objs"] = json::array();
  return j;
}

/** Build a SRAM descriptor for a cache-mode SRAM macro (iCache, dCache).
    Python runs CACTI cache mode; falls back to analytical 6T SRAM cell model. */
inline json
sram_cache(const std::string& label, size_t size_bytes,
           size_t block_bytes, size_t assoc) {
  json j;
  j["label"] = label;
  j["type"] = "sram";
  j["size"] = size_bytes;
  j["block_size"] = block_bytes;
  j["assoc"] = assoc;
  return j;
}

/** Build a SRAM descriptor for a RAM-mode SRAM macro (BTB, BPU tables).
    Python runs CACTI RAM mode; falls back to analytical 6T SRAM cell model. */
inline json
sram_ram(const std::string& label, size_t size_bytes,
         size_t word_bytes) {
  json j;
  j["label"] = label;
  j["type"] = "sram";
  j["size"] = size_bytes;
  j["word_size"] = word_bytes;
  return j;
}

/** Build a SRAM descriptor for a hard macro with known bit dimensions.
    Python computes area directly using the analytical 6T cell model,
    matching the gen_sram_lib.py formula used by the .lib file for STA. */
inline json
sram_macro(const std::string& label, size_t total_bits) {
  json j;
  j["label"] = label;
  j["type"] = "sram_macro";
  j["total_bits"] = total_bits;
  return j;
}

} // namespace area
