#include "branchSim/Tage.hh"
#include "defines/debug.hh"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>

using namespace branchSim;

// ============================================================================
// TagePredictor -- construction
// ============================================================================

TagePredictor::TagePredictor(const std::string& name, size_t base_pow2,
                             int comp_pow2, std::vector<int> hist_lens)
    : BranchPred(name)
    , n_comp_(std::min((int)hist_lens.size(), MAX_COMP))
    , hist_lens_param_(hist_lens.begin(),
                       hist_lens.begin() +
                         std::min((int)hist_lens.size(), MAX_COMP))
    , tag_mask_((1u << TAG_BITS) - 1u)
    , base_table_(1u << base_pow2, 1u)
    , base_mask_((1u << base_pow2) - 1u)
    , base_idx_bits_(base_pow2)
    , ghr_(0)
    , ghr_mask_(0) {
  assert(n_comp_ >= 1);
  assert(comp_pow2 >= 1);

  for (int i = 0; i < n_comp_; i++) {
    hist_len_[i] = std::max(hist_lens_param_[i], 1);
    comp_idx_bits_[i] = comp_pow2;
    comp_mask_[i] = (1u << comp_pow2) - 1u;
    comp_tables_[i].assign(1u << comp_pow2, TageEntry{});
  }

  // GHR mask: keep only the longest history length bits.
  int max_hist = hist_len_[n_comp_ - 1];
  ghr_mask_ = (max_hist < 64) ? ((1ULL << max_hist) - 1ULL) : ~0ULL;

  std::memset(&tage_stats_, 0, sizeof(tage_stats_));
  std::memset(snap_ring_, 0, sizeof(snap_ring_));
}

// ============================================================================
// GHR folding
// ============================================================================

// Fold GHR[0..hist_len-1] to out_bits using XOR compression.
uint32_t
TagePredictor::fold_ghr(uint64_t ghr, int hist_len, int out_bits) {
  if (hist_len >= 64)
    hist_len = 63;
  uint64_t mask = (hist_len < 63) ? ((1ULL << hist_len) - 1ULL) : ~0ULL;
  ghr &= mask;
  uint32_t out_mask = (out_bits < 32) ? ((1u << out_bits) - 1u) : ~0u;
  uint32_t result = 0;
  while (ghr) {
    result ^= (uint32_t)(ghr & out_mask);
    ghr >>= out_bits;
  }
  return result & out_mask;
}

// Component index: fold(PC >> 2, idx_bits) XOR fold(GHR, L_i, idx_bits).
uint32_t
TagePredictor::comp_idx(addr_t pc, uint64_t ghr, int comp) const {
  int bits = comp_idx_bits_[comp];
  uint32_t pc_part = (uint32_t)((pc >> 2) & comp_mask_[comp]);
  uint32_t ghr_part = fold_ghr(ghr, hist_len_[comp], bits);
  return (pc_part ^ ghr_part) & (uint32_t)comp_mask_[comp];
}

// Component tag: two-function hash for lower collision probability.
// Uses LOWER PC bits combined with two CSR-folded GHR values.
// Avoids using upper PC bits which are zero for typical RISC-V 0x8000xxxx
// addresses, which would cause false tag hits between different branches.
uint16_t
TagePredictor::comp_tag(addr_t pc, uint64_t ghr, int comp) const {
  // CSR1: fold full GHR to TAG_BITS
  uint32_t h1 = fold_ghr(ghr, hist_len_[comp], TAG_BITS);
  // CSR2: fold right-shifted GHR to TAG_BITS-1 (avoids correlating with h1)
  uint32_t h2 = fold_ghr(ghr >> 1, hist_len_[comp] > 1 ? hist_len_[comp] - 1 : 1,
                          TAG_BITS - 1);
  // Lower PC bits [TAG_BITS+1:2]: guaranteed to vary between branches.
  uint32_t pc_low = (pc >> 2) & tag_mask_;
  return (uint16_t)((pc_low ^ h1 ^ (h2 << 1)) & tag_mask_);
}

// ============================================================================
// predict
// ============================================================================

BPUPredResult
TagePredictor::predict(addr_t pc, addr_t btb_target) {
  stats.accesses++;

  // Allocate a snapshot slot.
  uint32_t snap_id = snap_wr_idx_++;
  TageSnap& snap = snap_ring_[snap_id % SNAP_RING];
  snap.ghr_snap = ghr_;
  snap.btb_hit_at_pred = (btb_target != 0);

  // Base predictor lookup.
  size_t bi = base_index(pc);
  snap.base_cnt = base_table_[bi];
  snap.bimodal_taken = (snap.base_cnt >= 2);

  // Pre-compute indices and tags for all components.
  for (int i = 0; i < n_comp_; i++) {
    snap.fidx[i] = comp_idx(pc, ghr_, i);
    snap.ftag[i] = comp_tag(pc, ghr_, i);
    const TageEntry& e = comp_tables_[i][snap.fidx[i]];
    snap.comp_hit[i] = e.valid && (e.tag == snap.ftag[i]);
  }

  // Find provider (longest matching component) and alt (second longest).
  snap.provider = -1;
  snap.alt_provider = -1;
  for (int i = n_comp_ - 1; i >= 0; i--) {
    if (snap.comp_hit[i]) {
      if (snap.provider == -1) {
        snap.provider = i;
        const TageEntry& e = comp_tables_[i][snap.fidx[i]];
        snap.provider_idx = snap.fidx[i];
        snap.provider_ctr = e.ctr;
        snap.provider_u = e.u;
      } else if (snap.alt_provider == -1) {
        snap.alt_provider = i;
        const TageEntry& e = comp_tables_[i][snap.fidx[i]];
        snap.alt_idx = snap.fidx[i];
        snap.alt_ctr = e.ctr;
        snap.alt_taken = (e.ctr >= 0);
        break;
      }
    }
  }

  // If alt_provider not found in components, use base.
  if (snap.alt_provider == -1) {
    snap.alt_provider = -1; // already
    snap.alt_taken = snap.bimodal_taken;
    snap.alt_ctr = (int8_t)snap.base_cnt;
    snap.alt_idx = (uint32_t)bi;
  }

  // Final prediction: provider or base.
  bool tage_taken;
  if (snap.provider >= 0) {
    // Use a weak provider (|ctr| == 0 or 1 at boundary) conservatively:
    // if the provider is "freshly allocated" (u==0, ctr is weak), use alt.
    const TageEntry& pe = comp_tables_[snap.provider][snap.provider_idx];
    bool provider_weak = (pe.ctr == 0 || pe.ctr == -1) && (pe.u == 0);
    if (provider_weak && snap.alt_provider != snap.provider) {
      tage_taken = snap.alt_taken;
    } else {
      tage_taken = (snap.provider_ctr >= 0);
    }
  } else {
    tage_taken = snap.bimodal_taken;
  }

  snap.tage_taken = tage_taken;
  snap.valid = true;

  // Determine if TAGE overrides bimodal (direction differs with BTB hit).
  bool btb_hit = (btb_target != 0);
  bool tage_overrode = btb_hit && (tage_taken != snap.bimodal_taken);

  // Shift GHR speculatively with the predicted direction.
  // Only shift when BTB hit -- BTB-miss not-taken branches are treated as
  // non-branches for GHR purposes (they won't redirect PC and are hard to
  // track cleanly without SRAM port-conflict artefacts).
  // on_mispred() handles BTB-miss taken (misprediction) via snap.ghr_snap.
  if (btb_hit)
    ghr_ = ((ghr_ << 1) | (tage_taken ? 1ULL : 0ULL)) & ghr_mask_;

  DPRINTF(BranchPred,
          "TAGE Predict: PC=0x%x prov=%d alt=%d base_t=%d tage_t=%d "
          "override=%d snap=%u",
          pc, snap.provider, snap.alt_provider,
          (int)snap.bimodal_taken, (int)tage_taken,
          (int)tage_overrode, snap_id);

  // bht_cnt unused; ghr_snap carries snap_id for update/on_mispred.
  return {tage_taken, 0, snap_id, tage_overrode};
}

// ============================================================================
// update
// ============================================================================

void
TagePredictor::update(addr_t pc, bool taken, bool btb_hit, uint8_t /*old_cnt*/,
                      uint32_t snap_id) {
  // Detect snap ring overflow: if snap_wr_idx_ advanced >= SNAP_RING positions
  // past snap_id, the slot was overwritten by a later prediction.
  // This can happen when a branch is stalled at Decode for many cycles
  // (e.g., RAW dependency on an SDRAM cache miss, 123+ cycles).
  if ((snap_wr_idx_ - snap_id) >= (uint32_t)SNAP_RING) {
    tage_stats_.snap_ring_overflow++;
    return;
  }
  TageSnap& snap = snap_ring_[snap_id % SNAP_RING];
  if (!snap.valid) {
    tage_stats_.update_snap_invalid++;
    return;
  }

  // Update base predictor (standard 2-bit saturating counter).
  {
    size_t bi = base_index(pc);
    uint8_t& cnt = base_table_[bi];
    if (taken) {
      if (cnt < 3) cnt++;
    } else {
      if (cnt > 0) cnt--;
    }
  }

  // Update provider component counter.
  int prov = snap.provider;
  if (prov >= 0) {
    TageEntry& e = comp_tables_[prov][snap.provider_idx];
    update_ctr(e.ctr, taken);
  }

  // Determine predictions for usefulness update.
  bool prov_taken = (prov >= 0) ? (snap.provider_ctr >= 0) : snap.bimodal_taken;
  bool alt_taken = snap.alt_taken;

  // Update usefulness counter of provider:
  // increment when provider correct and alt wrong; decrement when reversed.
  if (prov >= 0 && prov_taken != alt_taken) {
    TageEntry& pe = comp_tables_[prov][snap.provider_idx];
    if (prov_taken == taken) {
      if (pe.u < U_MAX) pe.u++;
    } else {
      if (pe.u > 0) pe.u--;
    }
  }

  // Allocation on misprediction: use final TAGE prediction, not raw provider.
  // This avoids spurious allocations when the "weak provider -> use alt" path
  // made the final prediction correct even though prov_taken != taken.
  if (snap.tage_taken != taken) {
    tage_stats_.alloc_tried++;
    int start = prov + 1;
    if (start < n_comp_) {
      // Find the shortest candidate with u==0 above the provider.
      int alloc_comp = -1;
      for (int i = start; i < n_comp_; i++) {
        const TageEntry& e = comp_tables_[i][snap.fidx[i]];
        if (!e.valid || e.u == 0) {
          alloc_comp = i;
          break;
        }
      }
      if (alloc_comp < 0) {
        // All candidates have u > 0: decrement their u counters.
        for (int i = start; i < n_comp_; i++) {
          TageEntry& e = comp_tables_[i][snap.fidx[i]];
          if (e.valid && e.u > 0) e.u--;
        }
        tage_stats_.alloc_failures++;
      } else {
        TageEntry& e = comp_tables_[alloc_comp][snap.fidx[alloc_comp]];
        e.tag = snap.ftag[alloc_comp];
        e.ctr = taken ? 0 : -1; // weak: 0=taken, -1=not-taken
        e.u = 0;
        e.valid = true;
        tage_stats_.allocations++;
        DPRINTF(BranchPred, "TAGE Alloc: comp=%d idx=%u tag=0x%x ctr=%d",
                alloc_comp, snap.fidx[alloc_comp], e.tag, (int)e.ctr);
      }
    }
  }

  // Periodic usefulness reset: halve all u counters every U_RESET_PERIOD.
  u_reset_ctr_++;
  if (u_reset_ctr_ >= U_RESET_PERIOD) {
    u_reset_ctr_ = 0;
    for (int i = 0; i < n_comp_; i++)
      for (auto& e : comp_tables_[i])
        if (e.valid) e.u >>= 1;
  }

  snap.valid = false; // consume snapshot
}

// ============================================================================
// on_mispred -- restore GHR
// ============================================================================

void
TagePredictor::on_mispred(bool actual_taken, uint8_t /*old_cnt*/,
                           uint32_t snap_id) {
  if ((snap_wr_idx_ - snap_id) >= (uint32_t)SNAP_RING)
    return; // snap overwritten; GHR restore skipped (state already corrupted)
  TageSnap& snap = snap_ring_[snap_id % SNAP_RING];
  // Restore GHR: discard all speculative shifts after this branch,
  // then shift in the actual direction.
  ghr_ = ((snap.ghr_snap << 1) | (actual_taken ? 1ULL : 0ULL)) & ghr_mask_;
  DPRINTF(BranchPred, "TAGE GHR restore: snap_id=%u ghr=0x%lx actual=%d",
          snap_id, (unsigned long)ghr_, (int)actual_taken);
}

// ============================================================================
// on_resolved -- accuracy breakdown stats
// ============================================================================

void
TagePredictor::on_resolved(bool actual_taken, const BranchResult& pred) {
  uint32_t snap_id = pred.ghr_snap;
  if ((snap_wr_idx_ - snap_id) >= (uint32_t)SNAP_RING)
    return; // snap overwritten; skip stats for this branch
  TageSnap& snap = snap_ring_[snap_id % SNAP_RING];
  if (!snap.valid)
    return;

  // Total TAGE accuracy (final prediction vs actual direction).
  bool tage_ok = (snap.tage_taken == actual_taken);
  if (tage_ok)
    tage_stats_.tage_correct++;
  else
    tage_stats_.tage_mispred++;

  // Bimodal accuracy.
  if (snap.bimodal_taken == actual_taken)
    tage_stats_.bimodal_correct++;
  else
    tage_stats_.bimodal_mispred++;

  // TAGE override stats.
  if (pred.tage_overrode_bimodal) {
    tage_stats_.tage_overrides++;
    if (tage_ok)
      tage_stats_.tage_override_helpful++;
    else
      tage_stats_.tage_override_harmful++;
  }

  // Provider usage and per-component accuracy.
  if (snap.provider < 0) {
    tage_stats_.base_used++;
    if (tage_ok) tage_stats_.base_correct++;
    else         tage_stats_.base_mispred++;
  } else {
    int p = snap.provider;
    tage_stats_.comp_used[p]++;
    if (tage_ok) tage_stats_.comp_correct[p]++;
    else         tage_stats_.comp_mispred[p]++;
  }
}

// ============================================================================
// config_json / extra_stats_json
// ============================================================================

json
TagePredictor::config_json() const {
  json j;
  j["base_entries"] = base_table_.size();
  j["n_comp"] = n_comp_;
  json comps = json::array();
  for (int i = 0; i < n_comp_; i++) {
    comps.push_back({{"entries", comp_tables_[i].size()},
                     {"hist_len", hist_len_[i]},
                     {"idx_bits", comp_idx_bits_[i]},
                     {"tag_bits", TAG_BITS}});
  }
  j["components"] = comps;
  j["hist_lens"] = hist_lens_param_;

  // Area: rough DFF estimate.
  size_t base_bits = base_table_.size() * 2;
  size_t comp_entry_bits = (size_t)(CTR_MAX - CTR_MIN + 1) + 2 + TAG_BITS + 1;
  size_t comp_bits = 0;
  for (int i = 0; i < n_comp_; i++)
    comp_bits += comp_tables_[i].size() * comp_entry_bits;
  j["area"] = area::area_json(0.0, base_bits + comp_bits, 0.3);
  return j;
}

json
TagePredictor::extra_stats_json() const {
  json j;
  j["bimodal_correct"] = tage_stats_.bimodal_correct;
  j["bimodal_mispred"] = tage_stats_.bimodal_mispred;
  double total_br =
    (double)(tage_stats_.bimodal_correct + tage_stats_.bimodal_mispred);
  j["bimodal_miss_rate"] =
    total_br > 0 ? tage_stats_.bimodal_mispred / total_br : 0.0;
  j["tage_correct"] = tage_stats_.tage_correct;
  j["tage_mispred"] = tage_stats_.tage_mispred;
  j["tage_miss_rate"] = total_br > 0 ? tage_stats_.tage_mispred / total_br : 0.0;
  j["tage_overrides"]        = tage_stats_.tage_overrides;
  j["tage_override_helpful"] = tage_stats_.tage_override_helpful;
  j["tage_override_harmful"] = tage_stats_.tage_override_harmful;
  j["base_used"]    = tage_stats_.base_used;
  j["base_correct"] = tage_stats_.base_correct;
  j["base_mispred"] = tage_stats_.base_mispred;
  json cu = json::array(), cc = json::array(), cm = json::array();
  for (int i = 0; i < n_comp_; i++) {
    cu.push_back(tage_stats_.comp_used[i]);
    cc.push_back(tage_stats_.comp_correct[i]);
    cm.push_back(tage_stats_.comp_mispred[i]);
  }
  j["comp_used"]    = cu;
  j["comp_correct"] = cc;
  j["comp_mispred"] = cm;
  j["allocations"]          = tage_stats_.allocations;
  j["alloc_failures"]       = tage_stats_.alloc_failures;
  j["alloc_tried"]          = tage_stats_.alloc_tried;
  j["update_snap_invalid"]  = tage_stats_.update_snap_invalid;
  j["snap_ring_overflow"]   = tage_stats_.snap_ring_overflow;
  return j;
}
