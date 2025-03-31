/**
 * vp_dpe.hh
 *
 *  Author:   Pierre Ravenel
 * Created:   04/03/2024
 **/


#pragma once

#include <deque>
#include <iostream>
#include <vector>

#include "base/named.hh"
#include "base/statistics.hh"
#include "cpu/base.hh"
#include "cpu/cva6/VTAGE.hh"
#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/hmp.hh"
#include "cpu/cva6/vp_base.hh"
#include "debug/Cva6VP.hh"

namespace gem5 {
namespace cva6 {


struct hist_entry_t
{
  bool dir;
  bool path; // Path
  uint64_t id;
  bool commit = false;
  hist_entry_t(bool dir_=0, bool path_=0, uint64_t id_=0){
    dir = dir_;
    path = path_;
    id = id_;
  }
};

class ghist_t
{
  private:
  std::deque<hist_entry_t> hist;
  uint16_t base_size;

  public:
  ghist_t(uint16_t size) : base_size(size){
    for (uint16_t i = 0; i < size; i++){
      hist.push_front(hist_entry_t());
      hist[0].commit = true;
    }
  }

  void dump(){
    {
      std::ostringstream os;
      for (int i = 0; i < 64; i++){
        os << hist[i].dir;
      }
      DPRINTF(Cva6VP, "dir:  %s\n", os.str());
    }
    {
      std::ostringstream os;
      for (int i = 0; i < 64; i++){
        os << hist[i].path;
      }
      DPRINTF(Cva6VP, "path: %s\n", os.str());
    }
  }

  hist_entry_t i2h(Cva6DynInstPtr inst){
    assert(!inst->isBubble());
    bool taken = inst->predictedTaken;
    if (inst->execute_completed){
      taken = inst->pc_next_taken;
    }
    // TODO FIX PATH
    uint64_t path = (inst->pc->instAddr() >> 1) ^
                    (inst->pc->instAddr() >> 2) ^
                    (inst->pc->instAddr() >> 3);
    return hist_entry_t(taken, path, inst->id.fetchSeqNum);
  }

  bool insert(Cva6DynInstPtr inst){
    if (inst->triedToPredict){
      hist.push_front(i2h(inst));
      return true;
    }
    return false;
  }

  void commit(Cva6DynInstPtr inst){
    // printf("Commit id=%d\n", inst->id.fetchSeqNum);
    // for (hist_entry_t h: hist){
    //   printf("(%d/%d)->", h.dir, h.id);
    // }
    // printf("\n");

    if (inst->triedToPredict){
      assert(hist.size() > base_size);
      size_t idx = hist.size() - base_size -1;
      assert(hist[idx].id == inst->id.fetchSeqNum);
      hist[idx].commit = true;
      hist.pop_back();
    }
  }

  uint64_t getPath(uint64_t size=64){
    uint64_t res = 0;
    assert(hist.size() >= size);
    for (int i = 0; i < size; i++){
      res |= ((uint64_t)hist[i].path) << i;
    }
    return res;
  }

  uint64_t getDirFold(int original_length, int compressed_length){
    uint64_t res = 0;
    // printf("hist size = %d, ol = %d\n", hist.size(), original_length);
    assert(hist.size() >= original_length);
    for (int i = 0; i < original_length; i++){
      res ^= ((uint64_t)hist[i].dir) << (i % compressed_length);
    }
    return res;
  }

  std::deque<hist_entry_t>& getRaw(){
    return hist;
  }

  void flush(){
    while (1) {
      assert(hist.size());
      if (hist[0].commit){
        break;
      }
      hist.pop_front();
    }
    assert(hist.size() == base_size);
  }

  void flushfrom(Cva6DynInstPtr inst){
    if (inst->isBubble()){
      flush();
      return;
    }
    return; // flush from inst is impossible !
    hist_entry_t h = i2h(inst);
    while (1) {
      assert(hist.size());
      if (hist[0].id == h.id){ // Reach end, Fix history
        hist[0].dir = h.dir;
        hist[0].path = h.path;
        return;
      }
      if (hist[0].id < h.id){ // Reach end, exit
        return;
      }

      hist.pop_front();
    }
  }
};

// std::vector<unsigned> filterProbability;

class VPDPE : public Named
{
  protected:
    Cva6CPU &cpu;
    VP &vp;

    struct VPDPEStats : public statistics::Group
    {

      /** Stats */
      statistics::Scalar req;
      /* Compressor */
      statistics::Distribution cycles_pred_commit;
      statistics::Scalar compression_hit;
      statistics::Scalar up_hit;
      statistics::Scalar down_hit;
      /* Addr prediction corner */
      statistics::Scalar predaddr_taken;
      statistics::Scalar predaddr_takenhit;

      VPDPEStats(Cva6CPU &cpu, const std::string name) :
        statistics::Group(&cpu, name.c_str()),
        ADD_STAT(req, statistics::units::Count::get(),
                "Number of predictions"),
        ADD_STAT(cycles_pred_commit, statistics::units::Count::get(),
                "Delta Cycles between prediction and issue"),
        ADD_STAT(compression_hit, statistics::units::Count::get(),
                 "The CAM compressor is valid"),
        ADD_STAT(up_hit, statistics::units::Count::get(),
                 "The up address is valid"),
        ADD_STAT(down_hit, statistics::units::Count::get(),
                 "The down address is valid"),
        ADD_STAT(predaddr_taken, statistics::units::Count::get(),
                "Number of taken predicted addr"),
        ADD_STAT(predaddr_takenhit, statistics::units::Count::get(),
                "Number of taken+hit predicted addr")
      {
        cycles_pred_commit
          .init(0,16,1)
          .flags(statistics::pdf);
      }
    } stats;

    const int VP_DELAY = 2;
    std::deque<Cva6DynInstPtr> inflights;
    std::deque<Cva6DynInstPtr> issued;

    bool is_fresh_commited_values = false;

    bool TEST_MODE = false;
    bool DPE_IGNORE = false;
    int ISSUE_WIDTH;

    ghist_t ghist;
    HMP hmp;

  public:
    VPDPE(const std::string &name, Cva6CPU &cpu_,
          const BaseCva6CPUParams &params, VP& vp_) :
        Named(name),
        cpu(cpu_),
        vp(vp_),
        stats(cpu_, name),
        TEST_MODE(params.dpeTestMode),
        DPE_IGNORE(true), // params.dpeIgnore TODO ?
        ISSUE_WIDTH(params.issueWidth),
        ghist(640),
        hmp(name + ".hmp", cpu, params) { }

  protected:
    /** Internal predict */
    void predict(Cva6DynInstPtr inst);
    /** Returns true if instruction need execution */
    bool vp_perform_issue(Cva6DynInstPtr inst);

  public:
    /** Make prediction int the 2 DeltaCycle Window (at fetch) */
    void perform_window_predictions();

  public:
    /** 0) Insert decoded instruction */
    void insert(Cva6DynInstPtr inst);
    /** 1) Issue: Reurns true if instruction need execution */
    bool issue(Cva6DynInstPtr inst);
    /** 2) Commit: return true if flush is needed */
    bool commit(Cva6DynInstPtr inst);
    bool post_commit(Cva6DynInstPtr inst);

    /** x) Flush component */
    void flush(){
      inflights.clear();
      issued.clear();
      vp.flush();
      ghist.flush();
    }

    void flushfrom(Cva6DynInstPtr inst){
      ghist.flushfrom(inst);
      while (!issued.empty() && issued.back()->isAfterOrEqual(inst)){
          inflights.push_front(issued.back());
          issued.pop_back();
      }
    }

};

} // namespace cva6
} // namespace gem5

