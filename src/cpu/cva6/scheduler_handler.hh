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
      return *new NoScheduler();
    case 2:
      return *new SchedulerPierreMichaud(name, cpu, p);
  }
  fatal("Invalid Scheduler type: %d\n", p.schedType);
  return *new NoScheduler();
}

class SA
{
  public:
  /* The scheduler */
  BaseScheduler &scheduler;
  private:
  /* Renamming */
  PhysicalRegAllocator regalloc;
  uint64_t bbcnt = 0;

  public:
  SA(const std::string &name,
    Cva6CPU &cpu,
    const BaseCva6CPUParams &p) :
  scheduler(initSched(name, cpu, p)),
  regalloc(4096)
  { }
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
  bool can_push_scheduler(){ return regalloc.canRename(); }
  void push_scheduler(Cva6DynInstPtr inst){
    rename(inst);
    scheduler.push(inst);
  }
  bool can_pop_scheduled(){ return scheduler.canPop(); }
  Cva6DynInstPtr front_scheduler() { return scheduler.front(); }
  Cva6DynInstPtr pop_scheduler(){ return scheduler.pop(); }

  void commit(Cva6DynInstPtr inst){
    regalloc.commit(inst);
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

