/**
 * vp.hh
 *
 *  Author:   Pierre Ravenel
 * Created:   04/07/2023
 **/


#pragma once

#include <deque>
#include <iostream>
#include <vector>

#include "base/named.hh"
#include "base/statistics.hh"
#include "cpu/base.hh"
#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/vp_base.hh"
#include "debug/Cva6VP.hh"

namespace gem5 {
namespace cva6 {

struct lvp_entry_t
{
  uint64_t value = 0;
  confcpt_t conf;
};

// F$
typedef struct vp_fc_entry
{
  uint8_t *data = NULL;

  bool valid = false;
  uint64_t tag = 0;
  uint64_t conf = 0;

  uint8_t _write_by_store = 0;
  uint64_t _pc_writter = 0;
  uint8_t _updated = 0;
} vp_fc_entry_t;

typedef struct vp_inflight_store_cpt
{
  uint16_t cpt;  // log2(STORE_QUEUE)
}vp_inflight_store_cpt_t;

class VP : public Named
{
  public:
    struct VPStats : public statistics::Group
    {
      /** Stats */
      statistics::Scalar _hit_perfect; // hit count at commit time
      statistics::Scalar _hit_exp; // hit count at predict time
      statistics::Scalar _hitaddr_perfect; // hit count at commit time
      statistics::Scalar _hitaddr_exp; // hit count at predict time
      statistics::Scalar _hitaddrt1_exp;

      statistics::Scalar hit;
      statistics::Scalar miss;

      statistics::Scalar hitaddr;
      statistics::Scalar missaddr;

      statistics::Scalar req;

      statistics::Formula hitrate;
      statistics::Formula hitreqrate;
      statistics::Formula coverage;

      statistics::Scalar _hit_deter; // hit deter

      statistics::Scalar _hit_stra; // Stride alias
      statistics::Scalar _hit_stra_fca; // Stride alias and f$ alias

      statistics::Scalar _hit_wbs; // Write by store
      statistics::Scalar _hit_fca; // fake cache alias
      statistics::Scalar _hit_taga; // T1 tag alias


      statistics::Scalar compression_hit;
      statistics::Scalar up_hit;
      statistics::Scalar down_hit;
      statistics::Scalar addr_down_nup;




      VPStats(BaseCPU &cpu, const std::string name) :
        statistics::Group(&cpu, name.c_str()),
        ADD_STAT(_hit_perfect, statistics::units::Count::get(),
                 "Number of hit count at commit time"),
        ADD_STAT(_hit_exp, statistics::units::Count::get(),
                 "Number of hit count at predict time"),
        ADD_STAT(_hitaddr_perfect, statistics::units::Count::get(),
                 "Number of hit count at commit time"),
        ADD_STAT(_hitaddr_exp, statistics::units::Count::get(),
                 "Number of hit count at predict time"),
        ADD_STAT(_hitaddrt1_exp, statistics::units::Count::get(),
                 "Number of hit count at predict time"),
        ADD_STAT(hit, statistics::units::Count::get(),
                 "Number of hit taken"),
        ADD_STAT(miss, statistics::units::Count::get(),
                 "Number of miss taken"),
        ADD_STAT(hitaddr, statistics::units::Count::get(),
                 "Number of hit addr taken"),
        ADD_STAT(missaddr, statistics::units::Count::get(),
                 "Number of miss addr taken"),
        ADD_STAT(req, statistics::units::Count::get(),
                 "Number of load commited"),

        ADD_STAT(hitrate, statistics::units::Rate<
          statistics::units::Count, statistics::units::Count>::get(),
                 "hit / (hit + miss)"),
        ADD_STAT(hitreqrate, statistics::units::Rate<
          statistics::units::Count, statistics::units::Count>::get(),
                 "hit / req"),
        ADD_STAT(coverage, statistics::units::Rate<
          statistics::units::Count, statistics::units::Count>::get(),
                 "(hit + miss) / req"),
        ADD_STAT(_hit_deter, statistics::units::Count::get(),
                 "Number of deterministics hit"),
        ADD_STAT(_hit_stra, statistics::units::Count::get(),
                 "Number of hit pred when stride alias"),
        ADD_STAT(_hit_stra_fca, statistics::units::Count::get(),
                 "Number of hit pred when stride and f$ alias"),
        ADD_STAT(_hit_wbs, statistics::units::Count::get(),
                 "Number of hit pred Writed by store"),
        ADD_STAT(_hit_fca, statistics::units::Count::get(),
                 "Number of hit pred when fake cache alias"),
        ADD_STAT(_hit_taga, statistics::units::Count::get(),
                 "Number of hit pred when T1 alias"),

        ADD_STAT(compression_hit, statistics::units::Count::get(),
                 "The CAM compressor is valid"),
        ADD_STAT(up_hit, statistics::units::Count::get(),
                 "The up address is valid"),
        ADD_STAT(down_hit, statistics::units::Count::get(),
                 "The down address is valid"),
        ADD_STAT(addr_down_nup, statistics::units::Count::get(),
                 "The down address is valid but not the up")
      {
        hitrate.precision(6);
        hitrate = hit / (hit + miss);
        hitreqrate.precision(6);
        hitreqrate = hit / req;
        coverage.precision(6);
        coverage = (hit + miss) / req;
      }
    } stats;
    size_t size;

    size_t fcsize;
    size_t fclinesize;
    vp_fc_entry_t *avt;

    // std::deque<vp_inst_metadata_t*> inflights;

    struct vp_hist_conf_t
    {
      int8_t conf;
    };

    struct vp_addr_valid_t
    {
      bool valid;
    };

    vp_inflight_store_cpt_t *store_aliaser;
    uint64_t store_aliaser_size;

    vp_hist_conf_t *ht;
    vp_addr_valid_t *avvt;

    uint64_t last_pc;
    uint64_t hist;
  public:
    VP(const std::string &name, BaseCPU &cpu, size_t _size)
        : Named(name), stats(cpu, name),
        size(_size) {
        store_aliaser_size = _size;

        // Fake cache 4k
        uint64_t cache_size = 4*1024;
        fclinesize = DTLBRequest::BASESIZE;
        fcsize = cache_size / fclinesize;
        avt = new vp_fc_entry_t[fcsize];
        for (int i = 0; i < fcsize; i++){
          avt[i].data = new uint8_t[fclinesize];
        }

        ht = new vp_hist_conf_t[size];
        avvt = new vp_addr_valid_t[size];

        store_aliaser = new vp_inflight_store_cpt_t[store_aliaser_size];
    }

  protected:
    uint64_t compute_key(uint64_t shr);

  public:
    /* Make a prediction */
    virtual bool predict(vp_inst_metadata_t *res, bool atcommit=false){
        return false;
    }
    /**
     * Update the prediction at issue time.
     * Return if the prediction should be taken or not.
     * Default is previous taken.
    */
    virtual bool update_prediction_at_issue(vp_inst_metadata_t *res,
      uint64_t addr){
      return res->taken;
    }

    /**
     * Update the predictor when store are issued.
    */
    virtual void store_issued(uint64_t pc, uint64_t base_addr){
      /* Default is nothing to do */
    }

    virtual void store_commit(uint64_t pc, uint64_t addr){
      /* Default is nothing to do */
    }

    /* Commit value prediction */
    virtual bool commit(uint64_t pc, uint64_t addr, uint16_t rsize,
      uint64_t real_val, vp_inst_metadata_t *pred, bool is_load) {
      return false;
    }

    /* Account predictions for statistics */
    void commitaccount(vp_inst_metadata_t *res, uint64_t real_val);

    /* Fake cache utils */
    uint64_t fc_tag(uint64_t addr){ return addr >> log2i(fclinesize); }
    uint16_t fc_offset(uint64_t addr){ return addr & (fclinesize -1); }
    vp_fc_entry_t *fc_get_entry(uint64_t addr){
        return &avt[fc_tag(addr) % fcsize];
    }

    /* Fake cache store aliaser */
    vp_inflight_store_cpt_t *fc_sa_get(uint64_t addr);
    bool fc_sa_is_dep(uint64_t addr);
    int fc_sa_increment(uint64_t addr);
    int fc_sa_decrement(uint64_t addr);
    void fc_sa_clear();

    /* Fake cache operations */
    bool fake_cache_write(uint64_t pc, uint64_t addr,
      uint16_t size, uint8_t *real_val,
      bool is_load, bool is_amo);
    bool fake_cache_read(uint64_t pc, uint64_t addr, uint16_t size,
      vp_inst_metadata_t *res);
    void fake_cache_setconf(uint64_t addr, bool valid);
    void fake_cache_clear_by_addr(uint64_t addr);

    void flush();

    bool isEnable() { return size != 0; }
};

class VP_VXXX: public VP
{
  BaseAddrPred &AP;
  bool meta_lvp_enable;
  lvp_entry_t *lvt;

  public:
    VP_VXXX(const std::string &name, BaseCPU &cpu, size_t _size,
      BaseAddrPred &_AP, bool enable_lvp):
      VP(name, cpu, _size),
      AP(_AP), meta_lvp_enable(enable_lvp) {
          lvt = new lvp_entry_t[_size];
      }
  virtual bool predict_readfc(vp_inst_metadata_t *res, bool atcommit=false);
  virtual void predict_base_addr(vp_inst_metadata_t *res);
  virtual bool predict(vp_inst_metadata_t *res, bool atcommit=false){
    predict_base_addr(res);
    return predict_readfc(res, atcommit);
  }
  virtual bool commit(uint64_t pc, uint64_t addr, uint16_t rsize,
      uint64_t real_val, vp_inst_metadata_t *pred, bool is_load);
  virtual bool update_prediction_at_issue(vp_inst_metadata_t *res,
    uint64_t addr);
  void store_issued(uint64_t pc, uint64_t addr);
  void store_commit(uint64_t pc, uint64_t addr);
};

/*
 * Compressed version of VP_XXX
 */
class VP_VXXX_C: public VP_VXXX
{
  // BaseAddrPred &AP;
  // BaseLVP lvp;

  /* The prediction entry type */
  typedef struct pc_id_entry
  {
    uint64_t id = 0;
  }pc_id_entry_t;
  /* The Decompressor entry type */
  typedef struct id_addr_entry
  {
    uint64_t addr = 0;
    int64_t cpt = 0;
  }id_addr_entry_t;

  pc_id_entry_t *pcidt;
  id_addr_entry_t *idadt;

  const uint64_t NB_ID = 32; // Ever use modulo
  const uint64_t DOWN_ADDR_SIZE = 20;

  public:
    VP_VXXX_C(const std::string &name, BaseCPU &cpu, size_t _size,
      BaseAddrPred &_AP, bool enable_lvp):
      VP_VXXX(name, cpu, _size, _AP, enable_lvp){
        pcidt = new pc_id_entry_t[_size];
        idadt = new id_addr_entry_t[NB_ID];
        for (int i = 0; i < NB_ID; i++){
          idadt[i].cpt = i;
        }
      }

  void predict_base_addr(vp_inst_metadata_t *res) override {
    uint64_t pc = res->_pc;
    // Predict LSBs
    VP_VXXX::predict_base_addr(res);
    // Predict MSBs
    uint64_t id = pcidt[foldlogn(pc >> 1, size)].id;
    DPRINTF(Cva6VP, "VP %016lx: ID is %ld\n", pc, id);
    assert(id < NB_ID);
    uint64_t baseaddr = idadt[id].addr;
    if (baseaddr != (res->t1_addr >> DOWN_ADDR_SIZE)){
      DPRINTF(Cva6VP, "VP %016lx: UPDATE BASE ADDR: %lx -> %lx\n",
        res->_pc, (res->t1_addr >> DOWN_ADDR_SIZE), baseaddr);
    } else {
      DPRINTF(Cva6VP, "VP %016lx: VALID BASE ADDR: %lx\n",
        res->_pc, baseaddr);
    }
    // Fix the first predicted addr
    res->t1_addr &= (1 << DOWN_ADDR_SIZE) - 1;
    res->t1_addr |= baseaddr << DOWN_ADDR_SIZE;
  }

  bool predict(vp_inst_metadata_t *res, bool atcommit=false){
    predict_base_addr(res);
    return predict_readfc(res, atcommit);
  }

  const int IDAT_CPT_MAX = 16;
  /** Return addr -> id. Returns true if the id is newly allocated */
  uint64_t addr_to_id(uint64_t up_addr, bool *allocated,
    uint64_t* old_up_addr){
    *allocated = false;
    // return foldlogn(up_addr, NB_ID):
    // CAM lookup
    uint64_t idx = -1;
    for (int i = 0; i < NB_ID; i++){
      if (idadt[i].addr == up_addr){ // Hit line: return
        idx = i;
        break;
      }
    }
    // printf("idx = %ld\n", idx);
    if (idx == -1){ // No hit: allocate
      // Find worst
      idx = 0; // default is 0
      for (int i = 1; i < NB_ID; i++){
        if (idadt[i].cpt < idadt[idx].cpt){
          idx = i;
        }
      }
      // printf("allocate idx = %ld\n", idx);
      // Allocate
      adct_update(idx); // Set LRU
      *allocated = true;
      *old_up_addr = idadt[idx].addr;
      idadt[idx].addr = up_addr;
    }
    assert(idx < NB_ID);
    return idx;
  }

  void adct_update(uint64_t id){
    // printf("update idx = %ld : %ld\n", id, idadt[id].cpt);
    uint64_t cptd = 0;
    for (int i = 0; i < NB_ID; i++){
      if (i != id) {
        if (idadt[i].cpt >= idadt[id].cpt){
          idadt[i].cpt -= 1;
          cptd += 1;
        }
      }
    }
    idadt[id].cpt += cptd;
  }

  bool commit(uint64_t pc, uint64_t addr, uint16_t rsize,
      uint64_t real_val, vp_inst_metadata_t *res, bool is_load) {
    // Commit base pred
    VP_VXXX::commit(pc, addr, rsize, real_val, res, is_load);
    // bool pred_valid = (res->pred_val == real_val);

    // Commit MSBs pred
    bool allocated;
    uint64_t old_up_addr;
    uint64_t up_addr = addr >> DOWN_ADDR_SIZE;
    uint64_t id = addr_to_id(up_addr, &allocated, &old_up_addr);
    adct_update(id);

    // Update prediction table
    pcidt[foldlogn(pc >> 1, size)].id = id;
    // Compression check
    if (!allocated){ // Compression match
      DPRINTF(Cva6VP, "VP %016lx: compression match : %lx -> %lx\n",
        pc, up_addr, id);
      // Nothing to do
    } else { // Compression missmatch
        DPRINTF(Cva6VP, "VP %016lx: compression missmatch : %lx -> %lx\n",
        pc, up_addr, id);
      // Free all entries in the fake cache
      fake_cache_clear_by_addr(old_up_addr);
    }

    /* Some stats */
    uint64_t mask = (1 << DOWN_ADDR_SIZE) - 1;
    bool up_hit = (addr >> DOWN_ADDR_SIZE) ==
      (res->t1_addr >> DOWN_ADDR_SIZE);
    bool down_hit = (addr & mask) == (res->t1_addr & mask);
    stats.compression_hit += !allocated;
    stats.up_hit += up_hit;
    stats.down_hit += down_hit;
    stats.addr_down_nup += down_hit && !up_hit;

    return false;
  }
  // void store_issued(uint64_t pc, uint64_t addr);
  // void store_commit(uint64_t pc, uint64_t addr);
};

class VP_LVP: public VP
{
  public:
  lvp_entry_t *lvt;
    VP_LVP(const std::string &name, BaseCPU &cpu, size_t _size):
      VP(name, cpu, _size){
        lvt = new lvp_entry_t[_size];
      }
    bool predict(vp_inst_metadata_t *res, bool atcommit=false) {
      uint64_t pc = res->_pc;
      uint64_t tag = (pc >> 1);
      lvp_entry_t *lve = &lvt[tag%size];
      res->pred_val = lve->value;
      res->taken = lve->conf.valid();
      return res->taken;
    }
    /* Commit value prediction */
    bool commit(uint64_t pc, uint64_t addr, uint16_t rsize, uint64_t real_val,
           vp_inst_metadata_t *res, bool is_load) {
      bool pred_valid = (res->pred_val == real_val);
      uint64_t tag = (pc >> 1);
      lvp_entry_t *lve = &lvt[tag%size];
      lve->value = real_val;
      lve->conf.update_conf(pred_valid);
      commitaccount(res, real_val);
      return 0;
    }
};


class VP_Str2D: public VP
{
  public:
  typedef struct str2D_entry
  {
    uint64_t value = 0;
    confcpt_t conf;
    uint64_t str[2];
  } str2D_entry_t;

  str2D_entry_t *lvt;
    VP_Str2D(const std::string &name, BaseCPU &cpu, size_t _size):
      VP(name, cpu, _size){
        lvt = new str2D_entry_t[_size];
      }
    bool predict(vp_inst_metadata_t *res, bool atcommit=false) {
      uint64_t pc = res->_pc;
      uint64_t tag = (pc >> 1);
      str2D_entry_t *e = &lvt[tag%size];
      res->pred_val = e->value + e->str[0];
      res->taken = e->conf.valid();
      return res->taken;
    }
    bool commit(uint64_t pc, uint64_t addr, uint16_t rsize, uint64_t real_val,
           vp_inst_metadata_t *res, bool is_load) {
      bool pred_valid = (res->pred_val == real_val);
      uint64_t tag = (pc >> 1);
      str2D_entry_t *e = &lvt[tag%size];
      int64_t new_str = real_val - e->value;
      if (abs(new_str) > ((1<<16)-1)){
        new_str = 0;
      }
      e->value = real_val;
      if (e->str[1] == new_str) { e->str[0] = e->str[1]; }
      else { e->str[1] = new_str; }
      e->conf.update_conf(pred_valid);
      commitaccount(res, real_val);
      return 0;
    }
};


class VP_DFCM: public VP
{
  public:
    BaseDFCM dfcm;

    VP_DFCM(const std::string &name, BaseCPU &cpu, size_t _size):
      VP(name, cpu, _size), dfcm(_size) { }

    bool predict(vp_inst_metadata_t *res, bool atcommit=false) {
      uint64_t pc = res->_pc;
      res->taken = dfcm.predict(pc, &res->pred_val);
      return res->taken;
    }
    bool commit(uint64_t pc, uint64_t addr, uint16_t rsize, uint64_t real_val,
           vp_inst_metadata_t *res, bool is_load) {
      commitaccount(res, real_val);
      dfcm.update_conf(pc, res->pred_val == real_val);
      return dfcm.commit(pc, res->pred_val, real_val);
    }
};

VP* vpinit(int64_t type, const std::string &name,
  BaseCPU &cpu, size_t size);

} // namespace cva6
} // namespace gem5

