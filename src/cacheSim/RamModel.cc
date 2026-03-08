// cacheSim/RamModel.cc
#include "cacheSim/RamModel.hh"

namespace memSim {

json
SdramModel::config_json() const {
  json j;
  j["type"] = "SdramModel";
  j["dev_mhz"] = DEV_MHZ;
  j["freq_ratio"] = freq_ratio_;
  j["n_banks"] = N_BANKS;
  j["t_rcd"] = T_RCD;
  j["t_rp"] = T_RP;
  j["t_rfc"] = T_RFC;
  j["cas"] = CAS;
  j["refresh_period"] = REFRESH_PERIOD;
  j["per_word"] = PER_WORD;
  j["rd_base"] = RD_BASE;
  j["act_cost"] = ACT_COST;
  j["pre_cost"] = PRE_COST;
  return j;
}

json
SdramModel::stats_json() const {
  json j;
  j["row_hit"] = row_hit_;
  j["row_miss"] = row_miss_;
  j["row_conflict"] = row_conf_;
  j["refresh_count"] = refresh_count_;
  j["ctrl_stall_dev"] = ctrl_stall_;
  return j;
}

} // namespace memSim
