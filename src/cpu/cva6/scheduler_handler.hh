/**
 * scheduler_handler.hh
 *
 *  Author:   Pierre Ravenel
 * Created:   12/11/2024
 **/


#pragma once

#include <string>
#include <vector>

#include "base/named.hh"
#include "base/statistics.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/scheduler.hh"
#include "cpu/cva6/store_set.hh"
#include "cpu/cva6/vp.hh"
#include "debug/Cva6Sched.hh"
#include "debug/Cva6SchedSched.hh"

namespace gem5 {
namespace cva6 {


inline BaseScheduler& initSched(
  const std::string &name,
  Cva6CPU &cpu,
  const BaseCva6CPUParams &p) {
  switch (p.schedType){
    case 0:
      return *new NoScheduler(name, cpu, p);
    case 2:
      return *new SchedulerPierreMichaud(name, cpu, p);
  }
  fatal("Invalid Scheduler type: %d\n", p.schedType);
  return *new NoScheduler(name, cpu, p);
}

class SA
{
  public:
  /* The scheduler */
  BaseScheduler &scheduler;
  private:
  StreamAnalyser isa; /* Input stream analyser */
  StreamAnalyser osa; /* Output stream analyser */

  public:
  /* An address predictor */
  BaseAddrPredDFCM apred;

  private:
  /* Renamming */
  PhysicalRegAllocator regalloc;
  uint64_t bbcnt = 0;

  struct Stats : public statistics::Group
  {
    statistics::Scalar apred_req;
    statistics::Scalar apred_conf;
    statistics::Scalar apred_hit;
    statistics::Scalar apred_conf_hit;
    statistics::Scalar apred_taken_hit;
    statistics::Scalar apred_taken_miss;

    Stats(Cva6CPU &cpu) :
      statistics::Group(&cpu, "SA"),
      ADD_STAT(apred_req, ""),
      ADD_STAT(apred_conf, ""),
      ADD_STAT(apred_hit, ""),
      ADD_STAT(apred_conf_hit, ""),
      ADD_STAT(apred_taken_hit, ""),
      ADD_STAT(apred_taken_miss, "")
    { }
  } stats;


  public:
  SA(const std::string &name,
    Cva6CPU &cpu,
    const BaseCva6CPUParams &p) :
    scheduler(initSched(name, cpu, p)),
    isa(cpu, "sa.i", false),
    osa(cpu, "sa.o", false),
    apred(p.vpSize),
    regalloc(4096),
    stats(cpu) { }

  private:
  void rename(Cva6DynInstPtr inst){
    inst->bb_idx = bbcnt;
    bbcnt += isBBend(inst);

    if (!inst->isFault()){
      /* Rename instruction : default is no renamming */
      BinaryRegisterFile rf;
      for (uint8_t i = 0; i < inst->staticInst->numSrcRegs(); i++) {
        RegId regid = inst->staticInst->srcRegIdx(i);
        if ((regid.classValue() != InvalidRegClass) && !rf.isSet(regid)){
          PhysicalReg reg(regid);
          reg.is_reg_dead = inst->exec_data.is_reg_dead[i];
          inst->regs_src_phy.push_back(reg);
          rf.set(regid);
        }
      }
      rf.clear();
      for (uint8_t i = 0; i < inst->staticInst->numDestRegs(); i++) {
        RegId regid = inst->staticInst->destRegIdx(i);
        if ((regid.classValue() != InvalidRegClass) && !rf.isSet(regid)){
          inst->regs_dst_phy.push_back(PhysicalReg(regid));
          rf.set(regid);
        }
      }
      /* Annotate missing Reg Dead */
      for (auto& reg: inst->regs_src_phy){
        if (rf.isSetRaw(reg.virt_reg_idx)){ /* Rf contains rd regs */
          reg.is_reg_dead = true;
        }
      }
    }
    /* Rename */
    regalloc.rename(inst);
  }

  public:
  bool can_push_scheduler(){
    return regalloc.canRename() && scheduler.canPush();
  }
  void push_scheduler(Cva6DynInstPtr inst){
    rename(inst);
    /* Perform address prediction */
    if (!inst->isFault() && inst->staticInst->isLoad() && apred.isEnable()){
      apred.predict(inst->pc->instAddr(), &inst->vp_data);
      inst->vp_data.is_predicted = true;
    }
    /* Schedule */
    scheduler.push(inst);
    /* Some static statistics */
    isa.commit(inst);
  }
  bool can_pop_scheduled(){ return scheduler.canPop(); }
  Cva6DynInstPtr front_scheduler() { return scheduler.front(); }
  Cva6DynInstPtr pop_scheduler(){
    Cva6DynInstPtr inst = scheduler.pop();
    /* Ensure stores are InO */
    if (!inst->isFault() && inst->staticInst->isStore()){
      static uint64_t oldid = 0;
      assert(inst->id.fetchSeqNum >= oldid);
      oldid = inst->id.fetchSeqNum;
    }
    /* Some static statistics */
    osa.commit(inst);
    return inst;
  }

  void commit(Cva6DynInstPtr inst){
    regalloc.commit(inst);
    /* APRED commit */
    if (!inst->isFault() && inst->staticInst->isLoad() && apred.isEnable()){
      uint64_t pc = inst->pc->instAddr();
      // uint64_t base_addr = inst->getSrcRegOperand(0);
      // uint64_t addr = inst->dreq->req->getPaddr();
      uint64_t addr = inst->vp_data.eff_addr;
      apred.commit(pc, addr, &inst->vp_data);
      bool valid = inst->vp_data.t1_addr == addr;
      apred.update_conf(pc, valid, &inst->vp_data);
      stats.apred_req += 1;
      stats.apred_conf += inst->vp_data.t1_isconf;
      stats.apred_hit += valid;
      stats.apred_conf_hit += inst->vp_data.t1_isconf && valid;
      stats.apred_taken_hit += inst->vp_data.addr_taken && valid;
    }
  }

  void misspredaddr(Cva6DynInstPtr inst, uint64_t eff_addr){
    assert(inst->vp_data.addr_taken);
    uint64_t pc = inst->pc->instAddr();
    apred.commit(pc, eff_addr, &inst->vp_data);
    apred.update_conf(pc, false, &inst->vp_data);
    stats.apred_taken_miss += 1;
  }

  void flushfrom(Cva6DynInstPtr inst){
    if (!inst->isBubble()){
      fatal("Must implem\n");
    }
    scheduler.flush();
    regalloc.flush();
  }

  bool canInterrupts(){
    return false; // TODO iq.canInterrupts()
  }

};


} // namespace cva6
} // namespace gem5

