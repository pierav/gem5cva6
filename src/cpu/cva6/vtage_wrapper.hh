/*
 * Authors: Pierre Ravenel
 */

#pragma once

#include "cpu/cva6/VTAGE.hh"
#include "cpu/cva6/vp.hh"

namespace gem5 {
namespace cva6 {

class VP_TAGE: public VP
{
  public:
    VTageVP vtage;
    FoldedHistories ch; // Folded version of global hsitory

    VP_TAGE(const std::string &name, BaseCPU &cpu,
      size_t _size):
      VP(name, cpu, _size),
      vtage(name + ".vtage", _size)
      { ch = vtage.initialch(); }

    void insert(vp_inst_metadata_t *res, ghist_t &ghist){
      /* Mark instruction with the speculative history */
      ch.updatefull(ghist);
      res->ch = ch;
    }

    bool predict(vp_inst_metadata_t *res, bool atcommit=false) {
      prediction_t pred;
      res->ch.dump();
      vtage.setch(res->ch);
      pred = vtage.lookup(res->_pc, &res->history);
      res->pred_val = pred.first;
      res->value_ready = pred.second == VTAGE_CONF_MAX;
      return res->value_ready;
    }
    /* Commit value prediction */
    bool commit(uint64_t pc, uint64_t addr, uint16_t rsize, uint64_t real_val,
           vp_inst_metadata_t *res, bool is_load) {
      bool pred_valid = (res->pred_val == real_val);
      vtage.update(real_val, &res->history, pred_valid, false);
      commitaccount(res, real_val);
      return 0;
    }

    // Do state less
    // void flush() override {} Nothing to do
};


} // namespace cva6
} // namespace gem5
