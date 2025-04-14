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

struct OoOCommitHandler
{
  struct Stats : public statistics::Group
  {
    statistics::Scalar lost;
    statistics::Scalar committed;
    statistics::Distribution committed_block_size;
    Stats(Cva6CPU &cpu) :
      statistics::Group(&cpu, "OOOCA"),
      ADD_STAT(lost, ""),
      ADD_STAT(committed, ""),
      ADD_STAT(committed_block_size, "")
    {
      committed_block_size
        .init(1,64,1)
        .flags(statistics::pdf);
    }
  } stats;

  OoOCommitHandler(Cva6CPU &cpu) :
    stats(cpu) {}

  std::deque<Cva6DynInstPtr> fifo;
  ArchRegFile<char> preg_in_flight;

  void clear(){
    fifo.clear();
    preg_in_flight.setall(false);
  }

  // Ideal commit Ino
  void commit(Cva6DynInstPtr& inst){
    /* Push and increment spec commit pointer */
    fifo.push_back(inst);
    if (inst->regs_dst_phy.size()){
      PhysicalReg& reg = inst->regs_dst_phy.front();
      bool inrf = inst->free_reg_at_commit;
      preg_in_flight[reg] = !inrf;
    }
    /* Try to push everything */
    if (preg_in_flight.isall(false)){ // All are ready to be commited
      stats.committed_block_size.sample(fifo.size());
      stats.committed += fifo.size();
      /* Reset */
      clear();
    }
  }

  void flush(){
    stats.lost += fifo.size();
    clear();
  }
};

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
  OoOCommitHandler oooch;

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
    oooch(cpu),
    schedRegBarrier(p.schedRegBarrier),
    stats(cpu) { }

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
  void push_scheduler(Cva6DynInstPtr inst){
    /* Schedule */
    scheduler.push(inst);
    /* Some static statistics */
    // isa.commit(inst);
    /* If required perform speculative free */
    regalloc.speculative_update(inst);

    // static uint64_t cntbranch = 0;
    // cntbranch += !inst->isFault() && inst->staticInst->isControl();
    // static uint64_t cpt = 0;
    // bool do_reg_barriere =
    //   (cpt++ % schedRegBarrier == (schedRegBarrier-1))
    // || needSerialise(inst);
    // if (do_reg_barriere){
    //   cpt = 0;
    //   DPRINTF(Cva6Sched, "Schedule reg barriere for %s\n", inst);
    //   // cpu.pipeline->sa.regalloc.reg_barrier();
    //   regalloc.reg_barrier();
    // }
    if (!inst->isFault() && inst->staticInst->isCondCtrl() &&
      !inst->isHighConf){
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

  void commit(Cva6DynInstPtr inst){
    regalloc.commit(inst);
    oooch.commit(inst); /* Must be after regalloc commit ! */
  }

  void flushfrom(Cva6DynInstPtr inst){
    if (!inst->isBubble()){
      fatal("Must implem\n");
    }
    scheduler.flush();
    regalloc.flush();
    oooch.flush();
  }

  bool canInterrupts(){
    return false; // TODO iq.canInterrupts()
  }

};


} // namespace cva6
} // namespace gem5

