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

class SA
{
  /* The scheduler */
  BaseScheduler &scheduler;
  /* Renamming */
  PhysicalRegAllocator regalloc;
  uint64_t bbcnt = 0;

  public:
  SA(const std::string &name,
    Cva6CPU &cpu,
    const BaseCva6CPUParams &p) :
  scheduler(*new SchedulerPierreMichaud(name, cpu, p)),
  regalloc(64)
 { }

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
  void push_at_decode(Cva6DynInstPtr inst){
    /* Push in scheduler */
    scheduler.push(inst);
  }

  bool can_push_at_decode(){
    return true;
  }
  bool can_pop_at_decode(){
    return scheduler.canPop();
  }

  Cva6DynInstPtr pop_at_decode(){
    return scheduler.pop();
  }

  void flushfrom(Cva6DynInstPtr inst){
    // fatal("Must implem\n");
  }

  bool canInterrupts(){
    return false; // TODO iq.canInterrupts()
  }

};


} // namespace cva6
} // namespace gem5

