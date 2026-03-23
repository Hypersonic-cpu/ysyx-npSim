#pragma once
#include "branchSim/BranchPred.hh"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace branchSim {

// ---- TagePredictor ---------------------------------------------------------
// TAgged GEometric history length predictor.
//
// Architecture:
//   - Base predictor: 2-bit saturating counter table indexed by folded PC.
//   - N tagged components T[0..N-1] with geometric history lengths.
//     History lengths: L[i] = ceil(L[0] * alpha^i), alpha ~ 2.
//     Default: N=4, lengths = {8, 16, 32, 64}.
//   - Each tagged entry: 3-bit signed counter (-4..3), 2-bit usefulness, tag.
//   - Prediction: longest-history matching component wins; base is fallback.
//
// Timing model (pipelined TAGE):
//   - predict() computes both bimodal (base) and full TAGE result.
//   - Returns TAGE result as the authoritative prediction.
//   - Sets tage_overrode_bimodal=true when TAGE direction != bimodal
//     direction AND the BTB has a hit (i.e., redirect differs).
//   - Pipeline.cc applies a 1-cycle stall when tage_overrode_bimodal is set,
//     modeling the 1-cycle TAGE pipeline latency vs bimodal.
//
// GHR management:
//   - GHR is shifted speculatively at predict() time with the PREDICTED dir.
//   - on_mispred() restores GHR from the snapshot: ghr = (snap << 1) | actual.
//
// Update:
//   - ghr_snap in BPUPredResult carries snap_id (index into snap_ring_).
//   - update() and on_mispred() look up TageSnap via snap_id.

class TagePredictor : public BranchPred {
public:
  // Maximum number of tagged components supported.
  static constexpr int MAX_COMP = 8;
  // Partial tag width (bits).
  static constexpr int TAG_BITS = 8;
  // Tagged component counter width (bits, signed): range [-4, 3].
  static constexpr int CTR_MAX = 3;
  static constexpr int CTR_MIN = -4;
  // Usefulness counter bits.
  static constexpr uint8_t U_MAX = 3;
  // Usefulness reset period (every N counter updates).
  static constexpr uint32_t U_RESET_PERIOD = (1u << 18);
  // Snapshot ring buffer size.
  // Must exceed max in-flight predictions including RAW-stalled branches.
  // SDRAM cache miss stall: 51 + 3*24 = 123 cycles. A branch stalled at
  // Decode can see many younger branch predictions before update() runs.
  // 512 gives safe margin beyond the worst-case SDRAM stall.
  static constexpr size_t SNAP_RING = 512;

private:
  // One entry in a tagged component.
  struct TageEntry {
    int8_t ctr = 0;    // 3-bit signed: taken if >= 0
    uint8_t u = 0;     // 2-bit usefulness
    uint16_t tag = 0;  // partial tag
    bool valid = false;
  };

  // Snapshot captured at predict() time; carried to update() via snap_id.
  struct TageSnap {
    bool valid = false;
    bool bimodal_taken = false;  // base predictor result
    bool tage_taken = false;     // final TAGE prediction
    bool btb_hit_at_pred = false; // BTB hit at predict time (GHR was shifted)
    uint8_t base_cnt = 1;        // base counter at predict time
    // Provider: component that gave the final prediction (-1 = base).
    int provider = -1;
    int alt_provider = -1;       // second-longest match (-1 = base)
    uint32_t provider_idx = 0;
    uint32_t alt_idx = 0;
    int8_t provider_ctr = 0;
    uint8_t provider_u = 0;
    int8_t alt_ctr = 0;
    bool alt_taken = false;
    uint64_t ghr_snap = 0;       // full GHR before this predict's shift
    // Pre-computed index/tag for each component at prediction time.
    // Used for correct update and allocation at EX time.
    uint32_t fidx[MAX_COMP] = {};
    uint16_t ftag[MAX_COMP] = {};
    bool comp_hit[MAX_COMP] = {};
  };

  // Per-branch accuracy breakdown (updated in on_resolved).
  struct TageStats {
    size_t bimodal_correct = 0;
    size_t bimodal_mispred = 0;
    size_t tage_correct = 0;          // final TAGE prediction correct
    size_t tage_mispred = 0;          // final TAGE prediction wrong
    size_t tage_overrides = 0;
    size_t tage_override_helpful = 0; // TAGE correct, bimodal would be wrong
    size_t tage_override_harmful = 0; // TAGE wrong, bimodal would be right
    size_t base_used = 0;             // fallback to base predictor
    size_t base_correct = 0;          // base correct predictions
    size_t base_mispred = 0;          // base mispredictions
    size_t comp_used[MAX_COMP] = {};  // times each component was provider
    size_t comp_correct[MAX_COMP] = {};
    size_t comp_mispred[MAX_COMP] = {};
    size_t allocations = 0;
    size_t alloc_failures = 0;        // all candidates had u > 0
    size_t alloc_tried = 0;           // entered allocation block
    size_t update_snap_invalid = 0;   // update() skipped due to !snap.valid
    size_t snap_ring_overflow = 0;    // snap slot clobbered before update()
  };

  // Number of active components (set at construction, <= MAX_COMP).
  int n_comp_;
  // History lengths per component (also stored as vector for config output).
  int hist_len_[MAX_COMP];
  std::vector<int> hist_lens_param_; // for config_json
  // Index bits per component (derived from comp table size).
  int comp_idx_bits_[MAX_COMP];
  // Component table size masks.
  size_t comp_mask_[MAX_COMP];
  // Tag mask.
  uint16_t tag_mask_;

  // Base predictor: 2-bit saturating counters.
  std::vector<uint8_t> base_table_;
  size_t base_mask_;
  size_t base_idx_bits_;

  // Tagged component tables.
  std::vector<TageEntry> comp_tables_[MAX_COMP];

  // Global history register (64-bit to support long histories).
  uint64_t ghr_;
  uint64_t ghr_mask_; // mask for max history length

  // Snapshot ring.
  TageSnap snap_ring_[SNAP_RING];
  uint32_t snap_wr_idx_ = 0;

  // Usefulness reset counter.
  uint32_t u_reset_ctr_ = 0;

  TageStats tage_stats_;

  // ---- Internal helpers ----

  // Fold GHR[0..hist_len-1] down to out_bits.
  static uint32_t fold_ghr(uint64_t ghr, int hist_len, int out_bits);

  // Compute component index: hash(PC, GHR[0..L_i-1]).
  uint32_t comp_idx(addr_t pc, uint64_t ghr, int comp) const;

  // Compute component partial tag.
  uint16_t comp_tag(addr_t pc, uint64_t ghr, int comp) const;

  // Base predictor index (XOR-folded, same convention as BimodalPredictor).
  size_t base_index(addr_t pc) const {
    size_t lo = (pc >> 2) & base_mask_;
    size_t hi = (pc >> (2 + base_idx_bits_)) & base_mask_;
    return lo ^ hi;
  }

  // Saturating increment/decrement for 3-bit signed TAGE counter.
  static void update_ctr(int8_t& ctr, bool taken) {
    if (taken) {
      if (ctr < CTR_MAX) ctr++;
    } else {
      if (ctr > CTR_MIN) ctr--;
    }
  }

public:
  // base_pow2: log2 of base predictor entries.
  // comp_pow2: log2 of entries per tagged component (default 7 = 128).
  // hist_lens: history lengths for each tagged component; size = n_comp.
  //   Default {2, 8, 32} gives 3 components calibrated for coremark.
  explicit TagePredictor(const std::string& name,
                         size_t base_pow2 = 9,
                         int comp_pow2 = 7,
                         std::vector<int> hist_lens = {2, 8, 32});

  BPUPredResult predict(addr_t pc, addr_t btb_target) override;

  // old_ghr is reused as snap_id for TAGE.
  void update(addr_t pc, bool taken, bool btb_hit, uint8_t old_cnt,
              uint32_t snap_id) override;

  // old_ghr is reused as snap_id for TAGE.
  void on_mispred(bool actual_taken, uint8_t old_cnt,
                  uint32_t snap_id) override;

  void on_resolved(bool taken, const BranchResult& pred) override;

  json config_json() const override;
  json extra_stats_json() const override;
};

} // namespace branchSim
