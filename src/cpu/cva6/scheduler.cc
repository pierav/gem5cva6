/**
 * scheduler.cc
 *
 *  Author:   Pierre Ravenel
 * Created:   12/11/2024
 **/

#include "cpu/cva6/scheduler.hh"
#include "cpu/cva6/pipeline.hh"

namespace gem5 {
namespace cva6 {

bool needSerialise(Cva6DynInstPtr inst){
  if (inst->isFault()) {
    return true;
  }
  if (inst->staticInst->isReadBarrier() || /* Fence */
      inst->staticInst->isWriteBarrier() || /* Fence */
      inst->staticInst->isSerializing()){ /* Instruction serial*/
    return true;
  }
  if (inst->staticInst->isAtomic() ||
      inst->staticInst->isStoreConditional()){
    return true;
  }
  // Serialisation of low conf branch
  // BAD !
  // if (inst->staticInst->isCondCtrl() &&
  //   !inst->isHighConf){
  //   return true;
  // }
  return false;
}
uint64_t instructioncoststatic(Cva6DynInstPtr inst){
  if (inst->isFault()){
    return 0;
  }
  if (inst->staticInst->isControl()){
    return 0;
  }
  if (inst->staticInst->isMemRef()){
    if (inst->staticInst->isLoad()){
      return 4;
    } else {
      return 0;
    }
  }
  if (inst->staticInst->isFloating()){
    return 2;
  }
  if (inst->staticInst->isInteger()){
    return 1;
  }
  return 0;
}

std::string dumpInstPreg(Cva6DynInstPtr& inst){
  std::ostringstream os;
  if (inst->isBubble()){
    os << "bubble";
  } else {
    for (int i = 0; i < NBBBQ; i++){
      if ((inst->bb_idx % NBBBQ) == i){
        os << " " << (inst->bb_idx % 10) << " ";
      } else {
        os << " . ";
      }
    }
    os << "0x" << std::hex << inst->pc->instAddr() << std::dec;
    os << " sn:" << inst->id.fetchSeqNum;
    os << ": ";
    if (inst->isFault()){
      os << "F: " << inst->getFault()->name();
    } else if (inst->staticInst) {
      os << std::setw(30) << std::left
        << inst->staticInst->disassemble(inst->pc->instAddr());
      os << "[";
      for (auto& reg: inst->regs_dst_phy){
        os << reg << ", ";
      }
      os << " <- ";
      for (auto& reg: inst->regs_src_phy){
        os << reg << ", ";
      }
      os << "]";
    }
  }
  return os.str();
}

// bool
// SchedulerPierreMichaud::needRename(PhysicalReg &reg){
//   /* Arch reg last read is after  */
//   return maxtimeoflasttouch[reg] > timeofregready[reg];
// }

uint64_t
SchedulerPierreMichaud::getSourceUseLine(PhysicalReg &reg){
  uint64_t ready_time = timeofregready[reg] > base_time ?
                      timeofregready[reg] - base_time : 0;
  // uint64_t end_time = maxtimeoflasttouch[reg] > base_time ?
  //                      maxtimeoflasttouch[reg] - base_time : 0;
  // return std::max(begin_time, end_time);
  return ready_time;
  #if 0
  int64_t schedule_line = s2d.size()-1;
  for (; schedule_line >= 0; schedule_line--){
    scheduler_entry_t& se = s2d[schedule_line];
    if (se.isRaW(reg)){
      assert(alt_line == (se.execution_latency(reg) + schedule_line));
      return se.execution_latency(reg) + schedule_line;
    }
  }
  fatal_if(alt_line != 0, "Altline must be 0: %d for %s", alt_line, reg);
  return 0; // Active line
  #endif
}

uint64_t
SchedulerPierreMichaud::find_inst_line(Cva6DynInstPtr inst){
  int64_t schedule_line = s2d.size()-1;
  for (; schedule_line >= 0; schedule_line--){
    scheduler_entry_t& se = s2d[schedule_line];
    if (se.contains(inst)){
      return schedule_line;
    }
  }
  return 0; /* Active line */
}

uint64_t
SchedulerPierreMichaud::getSLRR(Cva6DynInstPtr &inst){
  uint64_t schedule_line = 0;
  // RaW
  for (auto &reg: inst->regs_src_phy){
    uint64_t sli = getSourceUseLine(reg);
    DPRINTF(Cva6Sched, "Schedule (RR: %s): line %d T %d for %s\n",
      reg, sli, sli + base_time, dumpInstPreg(inst));
    schedule_line = std::max(schedule_line, sli);
  }
  // WaR and WaW
  for (auto &reg: inst->regs_dst_phy){
    if (reg.isRenammed){ // if not renammed No WaR and WaW
      uint64_t tlr = maxtimeoflasttouch[reg];
      if (tlr > base_time){ // Apply serialisation
        uint64_t sli = tlr - base_time;
        schedule_line = std::max(schedule_line, sli);
      }
    }
  }
  // Ignore RaR
  DPRINTF(Cva6Sched, "Schedule (RR:          ): line %d T %d for %s\n",
    schedule_line, schedule_line + base_time, dumpInstPreg(inst));
  return schedule_line;
}

uint64_t
SchedulerPierreMichaud::getSLMDP(Cva6DynInstPtr &inst){
  uint64_t schedule_line = 0;
  if (!inst->isFault() && inst->staticInst->isLoad()){
  // if (isMemLoad(inst, addr_load)){
    /* MDP */
    Cva6DynInstPtr store_inst = Cva6DynInst::bubble();;
    bool is_dep = mdp.checkInst(inst->pc->instAddr(), &store_inst);
    uint64_t mdp_sched_line = is_dep ? find_inst_line(store_inst) : 0;
    schedule_line = std::max(schedule_line, mdp_sched_line);
    if (is_dep){
        DPRINTF(Cva6Sched, "Schedule (MDP hit      ): line %d T %d for %s\n",
          mdp_sched_line, mdp_sched_line + base_time, dumpInstPreg(inst));
    }
    /* Ideal MDP */
    // Cva6DynInstPtr real_store_inst = Cva6DynInst::bubble();
    // uint64_t ideal_mdp_sched_line = getScheduleLineForLoadAddr(addr_load,
    //   &real_store_inst);
    // stats.mdp_false_positive += mdp_sched_line > ideal_mdp_sched_line;
    // stats.mdp_true_positive += (mdp_sched_line == ideal_mdp_sched_line)
    //   && is_dep;
    // stats.mdp_true_negative += (mdp_sched_line == ideal_mdp_sched_line)
    //   && !is_dep;
    // stats.mdp_false_negative += mdp_sched_line < ideal_mdp_sched_line;
    // DPRINTF(Cva6Sched, " | REAL memRaW line %d : %s\n",
    //   ideal_mdp_sched_line, dumpInstPreg(real_store_inst));
    // DPRINTF(Cva6Sched, " | MDP predict line %d : %s\n",
    //       mdp_sched_line, dumpInstPreg(store_inst));

    // /* Anomaly ! */
    // if (mdp_sched_line < ideal_mdp_sched_line){
    //   assert(!real_store_inst->isBubble());
    //   // TODO reverse
    //   mdp.violation(real_store_inst->pc->instAddr(), inst->pc->instAddr());
    // }
    // /* Schedule with ideal */
    // stats.load_bypass_store += 0; // TODO
    // schedule_line = std::max(schedule_line, ideal_mdp_sched_line);
  }
  return schedule_line;
}

uint64_t
SchedulerPierreMichaud::getSLSTORE(Cva6DynInstPtr &inst){
  uint64_t schedule_line = 0;
  /* Store order : do not allow store store bypass */
  if (!inst->isFault() && inst->staticInst->isStore()){
    /* Is there a store dependancy */
    if (last_store_time > base_time){
      uint64_t store_schedule_line = last_store_time - base_time + 1;
      // +1 to avoid Store leak ??!
      schedule_line = std::max(schedule_line, store_schedule_line);
      DPRINTF(Cva6Sched, "Schedule (Store order  ): line %d T %d for %s\n",
        schedule_line, schedule_line + base_time, dumpInstPreg(inst));
    }
  }
  if (mla.isConstraints(inst)){
    uint64_t mla_time = mla.getMinSchedulerTime(inst);
    if (mla_time > base_time){
      uint64_t store_schedule_line = mla_time - base_time + 1;
      schedule_line = std::max(schedule_line, store_schedule_line);
      DPRINTF(Cva6Sched, "Schedule (Store NOLOCK ): line %d T %d for %s\n",
        schedule_line, schedule_line + base_time, dumpInstPreg(inst));
    }
  }
  return schedule_line;
}

uint64_t
SchedulerPierreMichaud::getSLBBdep(Cva6DynInstPtr &inst){
  return 0;
  uint64_t schedule_line = 0;
  uint64_t mlabb_time = mlabb.getMinSchedulerTime(inst);
    if (mlabb_time > base_time){
      schedule_line = mlabb_time - base_time + 1;
      DPRINTF(Cva6Sched, "Schedule (BB Deps      ): line %d T %d for %s\n",
        schedule_line, schedule_line + base_time, dumpInstPreg(inst));
    }
  return schedule_line;
}
uint64_t
SchedulerPierreMichaud::getScheduleLine(Cva6DynInstPtr inst, uint64_t &delta){
  uint64_t schedule_line = 0; // Active line
  delta = latency(inst); // Default latency

  /* 0) Generate serialisation point */
  if (needSerialise(inst)){
    schedule_line = s2d.size();
    last_serialisation_time = base_time + schedule_line;
    DPRINTF(Cva6Sched, "Serialise at line %d for %s\n",
      schedule_line, dumpInstPreg(inst));
    inst->needSerialise = true; /* Mark isntruction to be serialised */
    return schedule_line;
  }

  /* 1) Apply serialisation */
  if (last_serialisation_time > base_time){
    schedule_line = last_serialisation_time - base_time;
  }

  uint64_t sl_rr = getSLRR(inst);
  uint64_t sl_mdp = getSLMDP(inst);
  uint64_t sl_st = getSLSTORE(inst);
  uint64_t sl_bb = getSLBBdep(inst);

  /* Add our wip constraint */
  // if (mldabb.isConstraints(inst)){
  //   uint64_t mla_time = mldabb.getMinSchedulerTime(inst);
  //   if (mla_time > base_time){
  //     uint64_t mldabb_schedule_line = mla_time - base_time + 1;
  //     schedule_line = std::max(schedule_line, mldabb_schedule_line);
  //   }
  // }
  if (!inst->isFault() && inst->staticInst->isLoad()){
    /* If load prediction is confident remove reg deps */
    /* Also do not mark prediction if useless (sl_rr > sl_mdp)*/
    if (inst->vp_data.addr_ready && (sl_mdp < sl_rr)){
      // delta += (sl_rr - sl_mdp); // The defautl schedule
      schedule_line = std::max({schedule_line, sl_mdp, sl_bb});
       DPRINTF(Cva6Sched, "Schedule (ADDR PRED    ): line %d T %d for %s\n",
        schedule_line, schedule_line + base_time, dumpInstPreg(inst));
      inst->vp_data.addr_taken = true; /* Mark taken */
    } else {
      schedule_line = std::max({schedule_line, sl_rr, sl_mdp, sl_bb});
    }
  } else if (!inst->isFault() && inst->staticInst->isStore()) {
    schedule_line = std::max({schedule_line, sl_rr, sl_st, sl_bb});
  } else {
    schedule_line = std::max({schedule_line, sl_rr, sl_bb});
  }
  return schedule_line;
}

#if 0
uint64_t
SchedulerPierreMichaud::getScheduleLineForLoadAddr(uint64_t addr,
  Cva6DynInstPtr *store_inst){
  int64_t schedule_line = s2d.size()-1; /* Most recent line */
  for (; schedule_line >= 0; schedule_line--){
    scheduler_entry_t& se = s2d[schedule_line];
    if (se.isRaWMem(addr, store_inst)){
      return schedule_line;
    }
  }
  return 0; // Active line
}
#endif

void
SchedulerPierreMichaud::push(Cva6DynInstPtr inst) {
  inflight_insts_count += 1;
  /* Mdp things */
  if (!inst->isFault() && inst->staticInst->isStore()){
    mdp.pushStore(inst->pc->instAddr(), inst);
  }

  uint64_t delta;
  uint64_t schedule_line = getScheduleLine(inst, delta);
  if (delta > (size * 10)){ // Worst case size * load lat
    fatal("delta too big (delta=%d)!\n", delta);
  }
  DPRINTF(Cva6Sched, "Schedule (pre fix      ): line %d T %d : %s\n",
    schedule_line, base_time + schedule_line, dumpInstPreg(inst));

  /* Fix delta with Hit/Miss prediction */
  // BAD (+3%)
  // if (!inst->isFault() && inst->staticInst->isLoad() &&
  //     !inst->vp_data.hmp_l1hit_pred){
  //   delta = 20; // L1 Miss
  // }
  // if (!inst->isFault() && inst->staticInst->isLoad()){
  //   delta = 4 + inst->vp_data.hmp_proba;
  // }

  /* Ignore already filled lines */
  while (schedule_line < s2d.size() && !s2d[schedule_line].canPush()){
    schedule_line ++;
  }
  if (schedule_line >= s2d.size()){
    s2d.resize(schedule_line + 1);
  }

  /* Insert instruction */
  assert(s2d[schedule_line].canPush());
  s2d[schedule_line].push(inst, delta);
  /* Mark ready line */
  for (auto &reg: inst->regs_dst_phy){
    timeofregready[reg] = base_time + schedule_line + delta;
  }

  // WaR dep
  for (auto &reg: inst->regs_src_phy){
    maxtimeoflasttouch[reg] =
      std::max(maxtimeoflasttouch[reg], base_time + schedule_line);
  }
  // And WaW dep !
  for (auto &reg: inst->regs_dst_phy){
    maxtimeoflasttouch[reg] =
      std::max(maxtimeoflasttouch[reg], base_time + schedule_line);
  }

  /* Also try to VP : unlock register dependancy */
  inst->vp_data.value_taken = false;
  if (inst->vp_data.value_ready){
    assert(inst->staticInst);
    assert(inst->staticInst->isLoad());
    uint64_t value = inst->vp_data.pred_val;
    PhysicalReg &reg = inst->regs_dst_phy[0];
    /* Clear read dep */
    timeofregready[reg] = base_time;
    /* We also have to write predicted value to the PRF */
    cpu.pipeline->iq.forwardSpeculativeRegVal(reg, value);
    /* Mark the prediction taken */
    inst->vp_data.value_taken = true;
    DPRINTF(Cva6Sched, "SCHEDVP : UNLOCK %s with %lx\n",
      reg, value);
  }

  /* Mark store */
  if (!inst->isFault() && inst->staticInst->isStore()){
    last_store_time = base_time + schedule_line;
  }
  /* PR: TODO CARE BONUS ADD LOADS */
  // if (!inst->isFault() && inst->staticInst->isLoad()){
  //   last_store_time = std::max(last_store_time, base_time + schedule_line);
  // }
  DPRINTF(Cva6Sched, "SCHEDPUSH ::::::::::::::: line %d T %d : %s\n",
    schedule_line, base_time + schedule_line, dumpInstPreg(inst));

  mla.onSchedule(inst, base_time + schedule_line);
  // mldabb.onSchedule(inst, base_time + schedule_line);
  // mlabb.onSchedule(inst, base_time + schedule_line);

  // FIX ARRAY ! TODO NOT NEEDED (only when s2d is empty)
  while (!s2d.empty() && s2d.front().empty()){
    s2d.pop_front();
    base_time ++;
  }

  // Perform speculative register free

}

// void apply_reg_barrier(){
//   // Bend scheduling to alloc sometimes a valid arch state
//   static ArchRegFile<char> isinflights;
//   DPRINTF(Cva6Sched, "Schedule reg barriere for %s\n", inst);
//   uint64_t top_time = base_time + s2d.size();
//   for (int i = 0; i < cpu.pipeline->sa.regalloc.size(); i++){
//     if (isinflights[i]){
//       maxtimeoflasttouch[i] = top_time;
//     }
//   }
// }

Cva6DynInstPtr
SchedulerPierreMichaud::pop() {
  inflight_insts_count -= 1;
  /* Pop entry in scheduler */
  assert(s2d.size());
  scheduler_entry_t &se = s2d.front();
  assert(!se.empty());
  Cva6DynInstPtr inst = se.pop();
  /* Fix scheduler ring buffer */
  /* active_line ++ : Drop SE if clearred */
  while (!s2d.empty() && s2d.front().empty()){
    s2d.pop_front();
    base_time ++;
  }
  // DPRINTF(Cva6Sched, "size=%d, T=%d, #inflight=%d\n",
  //   s2d.size(), base_time, inflight_insts_count);
  /* Mdp things */
  if (!inst->isFault() && inst->staticInst->isStore()){
    mdp.popStore(inst->pc->instAddr(), inst);
  }
  DPRINTF(Cva6Sched, "SCHEDPOP: %s\n", dumpInstPreg(inst));
  return inst;
}

bool
SchedulerPierreMichaud::canRenameDest(Cva6DynInstPtr &inst,
  std::deque<uint64_t> &FL, uint64_t &preg) {
  bool hit = false;
  uint64_t max_schedule_time = 0;

  uint64_t delta;
  uint64_t schedule_line = getScheduleLine(inst, delta);
  uint64_t schedule_time = schedule_line + base_time;
  DPRINTF(Cva6Sched, "canRenameDest line %d T %d : %s\n",
    schedule_line, schedule_time, dumpInstPreg(inst));

  for (uint64_t pregi: FL){
    uint64_t preg_use_time = maxtimeoflasttouch[pregi];
    DPRINTF(Cva6Sched, "Try preg : %d : T=%d\n", pregi, preg_use_time);
    if (preg_use_time > schedule_time){
      /* Suboptimal schedule */
      continue;
    }
    if (!hit || (max_schedule_time < preg_use_time)){
      /* Use this line */
      max_schedule_time = preg_use_time;
      preg = pregi;
      hit = true;
    }
  }
  return hit;
}


}
}
