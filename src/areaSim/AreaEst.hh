#pragma once
#include <nlohmann/json.hpp>
#include <cstddef>

namespace area {

using json = nlohmann::ordered_json;

/** Build an area JSON node for a component whose area comes entirely from
    STA (timing_area_um2) and has no SRAM macros or explicit DFF counts.
    timing_bits defaults to 0 and timing_area is process-calibrated from STA. */
inline json
comb_only(double timing_area_um2) {
  json j;
  j["comb_percent"] = 0.0;
  j["timing_area"] = timing_area_um2;
  j["timing_bits"] = 0;
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

} // namespace area
