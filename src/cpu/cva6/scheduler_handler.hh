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
#include "cpu/cva6/pipeline.hh"
#include "cpu/cva6/preschedulers/schedulerbb.hh"
#include "cpu/cva6/preschedulers/schedulervp.hh"
#include "cpu/cva6/scheduler.hh"
#include "cpu/cva6/store_set.hh"
#include "cpu/cva6/vp.hh"
#include "debug/Branch.hh"
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
    // case 3:
    //   return *new SchedulerVP(name, cpu, p);
  }
  fatal("Invalid Scheduler type: %d\n", p.schedType);
  return *new NoScheduler(name, cpu, p);
}


class SA
{
  public:
  Cva6CPU &cpu;
  /* The scheduler */
  BaseScheduler &scheduler;
  private:
  // StreamAnalyser isa; /* Input stream analyser */
  // StreamAnalyser osa; /* Output stream analyser */

  public:
  /* An address predictor */

  public:
  /* Renamming */
  PhysicalRegAllocator regalloc;
  // OoOCommitHandler oooch;

  uint64_t schedRegBarrier;
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
    Cva6CPU &cpu_,
    const BaseCva6CPUParams &p) :
    cpu(cpu_),
    scheduler(initSched(name, cpu, p)),
    // isa(cpu, "sa.i", false),
    // osa(cpu, "sa.o", false),
    regalloc(
      name + ".rr", cpu, p,
      p.renameSize, p.renameIncArchReg,
      p.renameFreeRegDead, p.renameSpecRelease),
    // oooch(cpu),
    stats(cpu),
    fixer(cpu),
    rbh(name, cpu, p) { }

  public:
  bool can_push_scheduler(Cva6DynInstPtr inst){
    if (!regalloc.canRename() ||
       !scheduler.canPush(inst)){ // First check buffer capacitt
      return false;
    }
    // Rename src must success
    regalloc.rename_src(inst);
    if (inst->regs_dst_phy.size()){
      assert(inst->regs_dst_phy.size() == 1);
      auto &FL = regalloc.getFL();
      uint64_t preg;
      bool canRename = scheduler.canRenameDest(inst, FL, preg);
      if (!canRename){
        return false;
      }
      regalloc.rename_dst(inst, preg);
    }
    return true;
  }

  class fixer_t
  {
    Cva6CPU& cpu;
    uint64_t last_pc_fault = 0;
    uint64_t last_deltat;
    uint64_t cpt_bp, cpt_rb;
    std::unique_ptr<PCStateBase> last_pc_next_fault;
    bool last_pc_next_fault_taken;
    bool need_fix_bp = false;
    bool need_fix_rb = false;
    public:
    fixer_t(Cva6CPU& cpu_) : cpu(cpu_) {}

    void apply_fix_for(Cva6DynInstPtr &inst,
      bool fixbp, bool dorb, uint64_t deltat);
    void before_predict(Cva6DynInstPtr &inst);
    void after_predict(Cva6DynInstPtr &inst);
    bool on_schedule_need_rb(Cva6DynInstPtr &inst);

    void clear_on_it(){
      need_fix_rb = false;
      need_fix_bp = false;
      last_pc_fault = false;
    }

  } fixer;

  class RegBarrierhandler
  {
    uint64_t cpt_inst = 0;
    uint64_t cpt_stores = 0;
    uint64_t cpt_branch = 0;

    uint64_t tringinsts;
    uint64_t tringstores;
    bool schedDisableRB;

    public:
    RegBarrierhandler(const std::string &name,
                      Cva6CPU &cpu_,
                      const BaseCva6CPUParams &p) :
      tringinsts(p.schedRegBarrier),
      tringstores(p.lsuSQCWidth), /* CARE HERE THE SQ SIZE !*/
      schedDisableRB(p.schedDisableRB)
      {}


    bool predict_squash(Cva6DynInstPtr& inst){
      if (inst->isFault() || !inst->staticInst->isControl()){
        return false;
      }
      return !inst->isHighConf;
    }

    void commit(Cva6DynInstPtr& inst){
    }

    bool on_push_need_rb(Cva6DynInstPtr& inst){
      /* Compute new scores */
      cpt_inst += 1;
      cpt_stores += !inst->isFault() && inst->staticInst->isStore();
      cpt_branch += !inst->isFault() && inst->staticInst->isControl();
      /* Compte triggers based on previous count */
      /* OPTIONAL : TODO: have to be fine tunnet */
      // bool trig_cpt = cpt_inst == tringinsts;
      /* MANDATORY : Deadlock otherwise ! */
      bool trig_stores = cpt_stores == tringstores;
      /* OPTIONAL : Avoid strong flushs */
      // bool trig_no_hc = !inst->isFault() &&
      //                   inst->staticInst->isCondCtrl() &&
      //                   !inst->isHighConf;
      // bool trig_branch = cpt_branch == tringinsts;

      bool trig_preg = predict_squash(inst);
      bool test = trig_stores  /* Mandatory to avoid deadlock !*/
                 || needSerialise(inst); /* MANDATORY */
      if (!schedDisableRB){
        test = test
             || trig_stores
             || (cpt_branch==16)
             || trig_preg;
      }
      return test;
    }

    void reset(){
      cpt_inst = 0;
      cpt_stores = 0;
      cpt_branch = 0;
    }
  } rbh;

  void push_scheduler(Cva6DynInstPtr inst){
    /* Serialise before fault to have a valid replay */
    // need serialsie before for AMO only ?
    if (fixer.on_schedule_need_rb(inst) || needSerialise(inst)){
      regalloc.reg_barrier();
    }
    /* Schedule */
    scheduler.push(inst);
    /* Some static statistics */
    // isa.commit(inst);
    /* If required perform speculative free */
    regalloc.speculative_update(inst);

    // TODO: ensure that the SQ will always contain enought space
    // static uint64_t cntbranch = 0;
    // cntbranch += !inst->isFault() && inst->staticInst->isControl();
    if (rbh.on_push_need_rb(inst)){
      rbh.reset();
      DPRINTF(Cva6Sched, "Schedule reg barriere for %s\n", *inst);
      regalloc.reg_barrier();
    }
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
    // osa.commit(inst);
    return inst;
  }

  /* Annotate if instruction can commit (rd ready) */
  void pre_commit(Cva6DynInstPtr& inst){
    for (PhysicalReg& reg: inst->regs_dst_phy){
      inst->free_reg_at_commit = regalloc.reg_available(reg);
    }
  }

  void commit(Cva6DynInstPtr inst){
    regalloc.commit(inst);
    rbh.commit(inst);
    // oooch.commit(inst); /* Must be after regalloc commit ! */
  }

  void flushfrom(Cva6DynInstPtr inst){
    if (!inst->isBubble()){
      fatal("Must implem\n");
    }
    scheduler.flush();
    regalloc.flush();
    // oooch.flush();
  }

  bool canInterrupts(){
    return false; // TODO iq.canInterrupts()
  }

};


} // namespace cva6
} // namespace gem5

