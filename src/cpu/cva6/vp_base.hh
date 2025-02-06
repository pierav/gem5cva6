/**
 * vp_base.hh
 *
 *  Author:   Pierre Ravenel
 * Created:   27/12/2023
 **/


#pragma once

#include <deque>
#include <iostream>
#include <vector>

#include "base/named.hh"
#include "base/statistics.hh"
#include "cpu/base.hh"

namespace gem5 {
namespace cva6 {

#define CONF_MAX 32

struct confcpt_t
{
  int64_t conf = 0;
  void update_conf(bool valid){
    if (valid && conf < CONF_MAX){
      conf += 1;
    }
    if (!valid && conf > 0){
      conf = 0;
      // conf -= 1;
    }
  }
  bool valid(){
    return conf == CONF_MAX;
  }
};

uint64_t foldn(uint64_t v, uint8_t n);
uint64_t foldlogn(uint64_t v, size_t size);
int64_t fixstride(int64_t stride);

struct vp_inst_metadata_t
{

  // Input
  uint64_t inst_mem_req_imm = 0;
  uint64_t inst_mem_req_size = 0;
  uint64_t _pc;

  void init(uint64_t pc, uint64_t size, uint64_t imm){
    _pc = pc;
    inst_mem_req_size = size;
    inst_mem_req_imm = imm;
  }

  // Prediction time
  bool is_predicted = false;
  uint64_t time_predict = 0;

  // Output Issue
  bool taken = false; // new bit in sbe
  bool _hit_deter = false;
  uint64_t pred_val = 0; // rd in sbe
  uint64_t time_issue = 0;

  // Output Commit
  bool hit = 0;

  // Inflight
  uint64_t t1_addr = 0;
  uint64_t t1_str = 0;
  bool t1_isconf = false;
  uint64_t eff_addr = 0;


  bool fc_isconf = false;


  uint64_t pred_val_vastra = 0;
  bool pred_val_vastra_valid = 0;
  uint64_t pred_val_lvp = 0;
  bool pred_val_lvp_valid = 0;

  bool addr_taken = false;
  bool isPredAddr(){ return pred_val_vastra && t1_isconf; }
  uint64_t getPredAddr(){ return t1_addr; }

  // METADATA for stats

  uint64_t _pred_addr_t1;

  bool _pred_valid = false; // Produced value is the real one
  bool _predperfect_valid = false; // Same but perfect

  uint8_t _is_stra = 0;
  uint8_t _is_wbs = 0;
  uint8_t _is_fca = 0;
  uint8_t _is_taga = 0;

  uint64_t _pap_history = 0;
  void reset(){
    // taken = false;
    // pred_val_vastra_valid = false;
    // pred_val_lvp_valid = false;
    hit = false;
    _hit_deter = false;
    // *this = {};
  }
};


class BaseAddrPred
{
  public:
    size_t size;
    BaseAddrPred(size_t _size) : size(_size) {}
    virtual bool predict(uint64_t pc, vp_inst_metadata_t *res) = 0;
    virtual bool commit(uint64_t pc, uint64_t addr,
      vp_inst_metadata_t *res) = 0;
    virtual void update_conf(uint64_t pc, bool valid,
      vp_inst_metadata_t *res) = 0;
    virtual bool isEnable() { return size != 0; }
};

class BasePred
{
  public:
    virtual bool predict(uint64_t pc, uint64_t *res) = 0;
    virtual bool commit(uint64_t pc, uint64_t real_val) = 0;
    virtual void update_conf(uint64_t pc, bool valid) = 0;
};

class BaseLVP : public BasePred
{
  public:
    typedef struct lvp_entry
    {
      int64_t value = 0;
      confcpt_t conf;
    } lvp_entry_t;

    size_t size;
    lvp_entry_t *vt;

    BaseLVP(size_t _size) : size (_size) {
      vt = new lvp_entry_t[_size];
    }

    bool predict(uint64_t pc, uint64_t *res){
      lvp_entry_t *e = &vt[(pc >> 1) % size];
      *res = e->value;
      return e->conf.valid();
    }

    bool commit(uint64_t pc, uint64_t real_val){
      vt[(pc >> 1) % size].value = real_val;
      return 0;
    }

    void update_conf(uint64_t pc, bool valid){
      vt[(pc >> 1) % size].conf.update_conf(valid);
    }
};

class BaseDFCM
{
  public:
    typedef struct dcfm_entry
    {
      int64_t value = 0;
      uint64_t hist = 0;
      confcpt_t conf;
    } dcfm_entry_t;

    typedef struct dcfmvalue_entry
    {
      int64_t stride = 0;
    } dcfmvalue_entry_t;

    size_t size;
    dcfm_entry_t *dt;
    dcfmvalue_entry_t *vt;
    BaseDFCM(size_t _size) : size (_size) {
      dt = new dcfm_entry_t[_size];
      vt = new dcfmvalue_entry_t[_size];
    }

    bool predict(uint64_t pc, uint64_t *res){
      dcfm_entry_t *e = &dt[(pc >> 1) % size];
      dcfmvalue_entry_t *ve = &vt[e->hist % size];
      *res = e->value + ve->stride;
      // printf("res = %ld = %ld + %ld\n", *res, e->value, ve->stride);
      return e->conf.valid();
    }

    bool commit(uint64_t pc, uint64_t pred_val, uint64_t real_val){
      dcfm_entry_t *e = &dt[(pc >> 1) % size];
      dcfmvalue_entry_t *ve = &vt[e->hist % size];

      int64_t new_str = real_val - e->value;
      new_str = fixstride(new_str);

      // if (abs(new_str) > ((1<<16)-1)){
      //   new_str = new_str & ((1<<16)-1);
      // }
      e->value = real_val;
      e->hist = (e->hist << 5) ^ foldlogn(new_str, size);
      ve->stride = new_str;
      return 0;
    }

    void update_conf(uint64_t pc, bool valid){
      uint64_t tag = (pc >> 1);
      uint64_t index0 = tag % size;
      dcfm_entry_t *e = &dt[index0];
      e->conf.update_conf(valid);
    }
};


class BaseAddrPredDFCM : public BaseAddrPred
{
  public:
    BaseDFCM &dfcm;
    BaseAddrPredDFCM(size_t _size) :
      BaseAddrPred(_size),
      dfcm(*new BaseDFCM(_size)) {}

    bool predict(uint64_t pc, vp_inst_metadata_t *res){
      res->t1_isconf = dfcm.predict(pc, &res->t1_addr);
      return res->t1_isconf;
    }

    void update_conf(uint64_t pc, bool valid, vp_inst_metadata_t *res){
      dfcm.update_conf(pc, valid);
    }

    bool commit(uint64_t pc, uint64_t addr, vp_inst_metadata_t *res){
     return dfcm.commit(pc, res->t1_addr, addr);
    }
};

class BaseAddrPredASTRA : public BaseAddrPred
{
  public:

    typedef struct vp_vastra_entry
    {
      uint64_t addr;
      uint64_t str;
      confcpt_t conf;

      uint64_t lastvalue;
      uint64_t lastvalue_conf;
      uint64_t lastvalue_str[2];


      uint64_t _pc_writter;
    } vp_vastra_entry_t;

    typedef struct vp_vastra_str_entry
    {
      uint64_t str;
      confcpt_t conf;
      uint64_t _pc_writter;
    } vp_vastra_str_entry_t;

    vp_vastra_entry *at;
    vp_vastra_str_entry_t *st;

    BaseAddrPredASTRA(size_t _size) : BaseAddrPred(_size) {
      at = new vp_vastra_entry[size];
      st = new vp_vastra_str_entry_t[size];
    }

    bool predict(uint64_t pc, vp_inst_metadata_t *res){
      uint64_t tag = (pc >> 1);
      vp_vastra_entry_t *ae = &at[foldlogn(tag, size)];
      uint64_t old_str = ae->str;
      uint64_t old_addr = ae->addr;

      vp_vastra_str_entry_t *se = &st[old_str % size];
      uint64_t pred_addr = old_addr + se->str; // predicted addr

      // Result
      res->_is_taga = res->_pc != ae->_pc_writter;
      res->_is_stra = old_str != se->str &&
                      res->_pc != se->_pc_writter;
      res->t1_addr = pred_addr;
      res->t1_isconf = ae->conf.valid();
      return res->t1_isconf;
    }

    void update_conf(uint64_t pc, bool valid, vp_inst_metadata_t *res){
      uint64_t tag = (pc >> 1);
      vp_vastra_entry_t *ae = &at[foldlogn(tag, size)];
      ae->conf.update_conf(valid);
    }

    bool commit(uint64_t pc, uint64_t addr, vp_inst_metadata_t *res){
      // Check prediction
      // bool addr_valid = (res->t1_addr == addr);

      uint64_t tag = (pc >> 1);
      vp_vastra_entry_t *ae = &at[foldlogn(tag, size)];
      vp_vastra_str_entry_t *se = &st[ae->str % size];

      uint64_t new_str = addr - ae->addr;
      new_str = fixstride(new_str);
      ae->addr = addr;
      ae->str = new_str;
      ae->_pc_writter = pc;

      // Second level
      se->str = new_str;
      se->_pc_writter = pc;

      // bool pred_valid_vastra = res->pred_val_vastra == real_val;
      //   if (addridxok){
      //       if (pred_valid_vastra && ae->conf < CONF_MAX){
      //           ae->conf += 1;
      //       }
      //       if (!pred_valid_vastra && ae->conf > 0){
      //           ae->conf -= 1;
      //       }
      //   }
      return 0;
    }
};


class BaseAddrPredCAP : public BaseAddrPred
{
  public:
    typedef struct acap_entry
    {
        uint64_t addr = 0;
    } acap_entry_t;

    typedef struct acap_lb_entry
    {
        uint64_t str = 0;
        uint64_t history = 0;
        confcpt_t conf;
    }acap_lb_entry_t;

    acap_entry_t *lt; // Link table
    acap_lb_entry_t *lb; // Load Buffer

    BaseAddrPredCAP(size_t _size) : BaseAddrPred(_size) {
      lt = new acap_entry[size];
      lb = new acap_lb_entry[size];
    }

    bool predict(uint64_t pc, vp_inst_metadata_t *res){
      uint64_t tag = (pc >> 1);
      // Compute index
      acap_lb_entry_t *lbe = &lb[tag % size];
      acap_entry_t *e = &lt[lbe->history % size];
      uint64_t pred_addr = e->addr; //  + lbe->str;
      // Result
      res->t1_addr = pred_addr;
      res->t1_isconf = lbe->conf.valid();
      return res->t1_isconf;
    }

    void update_conf(uint64_t pc, bool valid, vp_inst_metadata_t *res){
      uint64_t tag = (pc >> 1);
      acap_lb_entry_t *e = &lb[tag % size];
      e->conf.update_conf(valid);
    }

    bool commit(uint64_t pc, uint64_t addr, vp_inst_metadata_t *res){
      uint64_t tag = (pc >> 1);
      acap_lb_entry_t *lbe = &lb[tag % size];
      acap_entry_t *e = &lt[lbe->history % size];

      // uint64_t new_str = addr - e->addr;
      lbe->history = (lbe->history << 5) ^ foldlogn(addr, size);
      // lbe->str = new_str;
      e->addr = addr;
      return 0;
    }
};


/*
* Load Value Prediction via Path-based Address Prediction:
* Avoiding Mispredictions due to Conflicting Stores
*/
class BaseAddrPredPAP : public BaseAddrPred
{
  public:
    typedef struct pap_entry
    {
        uint64_t addr = 0;
        confcpt_t conf;
    } pap_entry_t;

    pap_entry_t *t; // pap table
    uint64_t history;

    BaseAddrPredPAP(size_t _size) : BaseAddrPred(_size) {
      t = new pap_entry_t[size];
    }

    size_t compute_index(uint64_t pc, uint64_t hist){
      return ((pc >> 1) ^ (hist)) % size;
    }

    bool predict(uint64_t pc, vp_inst_metadata_t *res){
      pap_entry_t *e = &t[compute_index(pc, history)];

      // Update history at predict
      res->_pap_history = history;
      history = (history << 1) ^ foldn(pc, 1);

      uint64_t pred_addr = e->addr;
      res->t1_addr = pred_addr;
      res->t1_isconf = e->conf.valid();
      return res->t1_isconf;
    }

    void update_conf(uint64_t pc, bool valid, vp_inst_metadata_t *res){
      pap_entry_t *e = &t[compute_index(pc, res->_pap_history)];
      e->conf.update_conf(valid);
    }

    bool commit(uint64_t pc, uint64_t addr, vp_inst_metadata_t *res){
      pap_entry_t *e = &t[compute_index(pc, res->_pap_history)];
      e->addr = addr;
      return 0;
    }
};

class BaseAddrPredLVP : public BaseAddrPred
{
  public:
    typedef struct lvp_entry
    {
        uint64_t addr = 0;
        confcpt_t conf;
    } lvp_entry_t;

    lvp_entry_t *t; // pap table

    BaseAddrPredLVP(size_t _size) : BaseAddrPred(_size) {
      t = new lvp_entry_t[size];
    }

    bool predict(uint64_t pc, vp_inst_metadata_t *res){
      uint64_t tag = (pc >> 1);
      lvp_entry_t *e = &t[tag % size];
      res->t1_addr = e->addr;
      res->t1_isconf = e->conf.valid();
      return res->t1_isconf;
    }

    void update_conf(uint64_t pc, bool valid, vp_inst_metadata_t *res){
      uint64_t tag = (pc >> 1);
      lvp_entry_t *e = &t[tag % size];
      e->conf.update_conf(valid);
    }

    bool commit(uint64_t pc, uint64_t addr, vp_inst_metadata_t *res){
      uint64_t tag = (pc >> 1);
      lvp_entry_t *e = &t[tag % size];
      e->addr = addr;
      return 0;
    }
};

} // namespace cva6
} // namespace gem5

