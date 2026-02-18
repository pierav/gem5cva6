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


class SA : public ForwardInstDataPopIntf
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

    /* Serialise before fault to have a valid replay */
    // We call RB before canRenameDest and rename_dst to allow in place reuse
    if (fixer.on_schedule_need_rb_noupdate(inst) || needSerialise(inst)){
      regalloc.reg_barrier();
    }

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
    bool on_schedule_need_rb_noupdate(Cva6DynInstPtr &inst);

    void clear_on_it(){
      need_fix_rb = false;
      need_fix_bp = false;
      last_pc_fault = false;
    }

  } fixer;

  class RegBarrierhandler
  {
    Cva6CPU& cpu;
    uint64_t cpt_inst = 0;
    uint64_t cpt_stores = 0;
    uint64_t cpt_branch = 0;

    uint64_t tringinsts;
    uint64_t tringbranch;
    uint64_t tringstores;
    bool schedDisableRB;

    public:
    RegBarrierhandler(const std::string &name,
                      Cva6CPU &cpu_,
                      const BaseCva6CPUParams &p) :
      cpu(cpu_),
      tringinsts(p.schedRegBarrier),
      tringbranch(p.schedRegBarrier),
      tringstores(p.lsuSQCWidth), /* CARE HERE THE SQ SIZE !*/
      schedDisableRB(p.schedDisableRB)
      {}

    bool predict_squash(Cva6DynInstPtr& inst){
      if (inst->isFault() || !inst->staticInst->isControl()){
        return false;
      }
      return !inst->isHighConf;
    }

    // uint64_t mean_cnt = 16;
    // uint64_t latest_cnt = 0;

    void commit(Cva6DynInstPtr& inst){
      // latest_cnt += !inst->isFault() && inst->staticInst->isControl();
      // if (inst->isASquash()){
      //   if (latest_cnt > tringbranch){
      //     latest_cnt = tringbranch;
      //   }
      //   mean_cnt = 7*mean_cnt/8 + 1*latest_cnt/8;
      //   if (mean_cnt <= 0){
      //     mean_cnt = 1;
      //   }
      //   // printf("New mean_cnt = %d\n", mean_cnt);
      //   assert(mean_cnt > 0);
      //   assert(mean_cnt < tringbranch);
      //   latest_cnt = 0;
      // }
    }

    bool on_push_need_rb(Cva6DynInstPtr& inst);

    void reset(){
      cpt_inst = 0;
      cpt_stores = 0;
      cpt_branch = 0;
    }
  } rbh;

  void push_scheduler(Cva6DynInstPtr inst){
    // fixer.on_schedule_need_rb(inst); // Only to Increment cnt
    // Just to be sure :)
    if (fixer.on_schedule_need_rb(inst) || needSerialise(inst)){
      regalloc.reg_barrier();
    }

    /* Schedule */
    scheduler.push(inst);
    /* Some static statistics */
    // isa.commit(inst);
    /* If required perform speculative free */
    // regalloc.speculative_update(inst);

    // TODO: ensure that the SQ will always contain enought space
    // static uint64_t cntbranch = 0;
    // cntbranch += !inst->isFault() && inst->staticInst->isControl();
    if (rbh.on_push_need_rb(inst)){
      rbh.reset();
      DPRINTF(Cva6Sched, "Schedule reg barriere for %s\n", *inst);
      regalloc.reg_barrier();
    }
  }

  /* *** ForwardInstDataPopIntf interface *** */
  bool canPop(){ return scheduler.canPop(); }
  Cva6DynInstPtr front() { return scheduler.front(); }
  Cva6DynInstPtr pop(){
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
  void flush(){ flushfrom(Cva6DynInst::bubble()); }
  size_t size() override { return scheduler.size(); }

  /* Annotate if instruction can commit (rd ready) */
  void pre_commit(Cva6DynInstPtr& inst){
    for (PhysicalReg& reg: inst->regs_dst_phy){
      inst->free_reg_at_commit = regalloc.reg_available(reg);
    }
  }

  // void commit(Cva6DynInstPtr inst){
  //   regalloc.commit(inst);
  //   // rbh.commit(inst);
  //   // oooch.commit(inst); /* Must be after regalloc commit ! */
  // }

  void flushfrom(Cva6DynInstPtr inst){
    scheduler.flush(); // Scheduler insts must be before inst
    regalloc.flushfrom(inst);
  }

  bool canInterrupts(){
    return false; // TODO iq.canInterrupts()
  }

  void tick(){
    scheduler.tick();
  }

};


} // namespace cva6
} // namespace gem5
