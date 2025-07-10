/**
 * scheduler.cc
 *
 *  Author:   Pierre Ravenel
 * Created:   12/11/2024
 **/

#include "cpu/cva6/scheduler.hh"
#include "cpu/cva6/pipeline.hh"
#include "cpu/cva6/scheduler_handler.hh"

namespace gem5 {
namespace cva6 {

void
SA::fixer_t::apply_fix_for(Cva6DynInstPtr &inst, bool fixbp, bool dorb,
  uint64_t deltat){
  last_pc_fault = inst->pc->instAddr();
  last_deltat = deltat;
  if (fixbp){

  }
  set(last_pc_next_fault, inst->pc_next);
  last_pc_next_fault_taken = inst->pc_next_taken;
  need_fix_bp = fixbp;
  need_fix_rb = dorb;
  cpt_bp = 0;
  cpt_rb = 0;
}

void
SA::fixer_t::before_predict(Cva6DynInstPtr &inst){
  /* Lets check the trigger */
  if (!need_fix_bp){
    return;
  }
  cpt_bp++;
  bool pc_match = last_pc_fault == inst->pc->instAddr();
  bool cpt_match = cpt_bp == last_deltat;
  if (cpt_match && pc_match){
    // !!! NOT ALWAYS THE SAME PATH
    // assert(pc_match); // debug  (Must be on the same path)
    if (!inst->isFault() && (inst->staticInst->isControl() ||
                            inst->staticInst->isSyscall())){
      // In case of branch, tell the BP to fix it.
      // This is mandatory to keep a valid GHR
      cpu.pipeline->bp.set_fix(*inst->pc,
        *last_pc_next_fault, last_pc_next_fault_taken);
    } else {
      // Fix it ourself ?
      inst->predictedTaken = last_pc_next_fault_taken;
      set(inst->predictedTarget, last_pc_next_fault);
    }
  } else {
    if (pc_match){ // Old false positive
      // warn("False positive trigerred!\n");
    }
  }
}

void
SA::fixer_t::after_predict(Cva6DynInstPtr &inst){
  // Ignore
  if (!need_fix_bp){
    return;
  }
  bool cpt_match = cpt_bp == last_deltat;
  if (cpt_match){
    need_fix_bp = false;
    /* Check fix */
    if (!inst->isFault() && inst->staticInst->isControl()){
      // assert(inst->predictedTaken == last_pc_next_fault_taken);
      // assert(*inst->predictedTarget == *last_pc_next_fault);
      // DPRINTF(Branch, "Fix branch from (taken:%d) %s to (taken:%s) %s\n",
      //   inst->predictedTaken, *inst->predictedTarget,
      //   last_pc_next_fault_taken, *last_pc_next_fault);
      // if (inst->staticInst->isDirectCtrl()){
      //   assert(inst->predictedTaken == last_pc_next_fault_taken);
      //   assert(*inst->predictedTarget == *last_pc_next_fault);
      // }
    }
  }
}

bool
SA::fixer_t::on_schedule_need_rb(Cva6DynInstPtr &inst){
  if (!need_fix_rb){
    return false;
  }
  cpt_rb++;
  bool cpt_match = cpt_rb == last_deltat;
  if (cpt_match){
    need_fix_rb = false;
    // assert(last_pc_fault == inst->pc->instAddr());
    return true;
  }
  return false;
}

bool
SA::RegBarrierhandler::on_push_need_rb(Cva6DynInstPtr& inst){
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
  bool trig_branch = cpt_branch == tringbranch;

  /* MDP fail trigger */
  // bool trig_mdp = false;
  // if (!inst->isFault() && inst->staticInst->isLoad()){
  //   trig_mdp = !cpu.pipeline->hcpred.predictIsHC(inst);
  // }

  bool trig_preg = predict_squash(inst);
  bool test = trig_stores  /* Mandatory to avoid deadlock !*/
              || needSerialise(inst); /* MANDATORY */
  if (!schedDisableRB){
    test = test
          || trig_stores
          || trig_branch
          || trig_preg;
  }
  return test;
}

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
  // Serialisation of low conf branch ?
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
    return 1;
  }
  if (inst->staticInst->isMemRef()){
    if (inst->staticInst->isLoad()){
      return 5;
    } else {
      return 0;
    }
  }

  const uint64_t FLOAT_LAT = 3;
  /* Otherwise get FU latency */
  switch (inst->staticInst->opClass()){
    case OpClass::No_OpClass: return 0; /* Csr, etc */
    case OpClass::IntAlu: return 1;
    case OpClass::IntMult: return 3;
    case OpClass::IntDiv: return 8;
    case OpClass::FloatAdd: return FLOAT_LAT;
    case OpClass::FloatCmp: return FLOAT_LAT;
    case OpClass::FloatCvt: return FLOAT_LAT;
    case OpClass::FloatMult: return FLOAT_LAT;
    case OpClass::FloatMultAcc: return FLOAT_LAT;
    case OpClass::FloatMisc: return FLOAT_LAT;
    case OpClass::FloatDiv: return 18;
    case OpClass::FloatSqrt: return 18;
    default:
      fatal("Unrecheable latency for %s\n", *inst);
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
  if (!reg.isRenammedValid){ // Arch reg file is ready
    return 0;
  }
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
SchedulerPierreMichaud::getSLFU(Cva6DynInstPtr &inst){
  uint64_t sched_line = 0;
  uint64_t ready_time = fumodel.getRT(inst);
  if (ready_time > base_time){
    sched_line = ready_time - base_time;
    DPRINTF(Cva6Sched, "Schedule (SL FU        ): line %d T %d for %s\n",
        sched_line, ready_time, dumpInstPreg(inst));
  }
  return sched_line;
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
  if (!inst->isFault() && inst->staticInst->isMemRef()){
    /* MDP */
    if (!inst->mdpinst->isBubble()){
      // Store may have leave the scheduler
      // uint64_t test_line = find_inst_line(inst->mdpinst);
      uint64_t mdp_sched_line = 0;
      if (inst->mdpinst->scheduled_time > base_time){
        mdp_sched_line = inst->mdpinst->scheduled_time - base_time;
      }
      // fatal_if(mdp_sched_line != test_line, "%d==%d\n",
      //   mdp_sched_line, test_line);
      schedule_line = std::max(schedule_line, mdp_sched_line);
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

    /* Is there a store dependancy : Force store -> load serilation*/
    // if (last_store_time > base_time){
    //   uint64_t store_schedule_line = last_store_time - base_time + 1;
    //   // +1 to avoid Store leak ??!
    //   schedule_line = std::max(schedule_line, store_schedule_line);
    //   DPRINTF(Cva6Sched, "Schedule (MDP S order  ): line %d T %d for %s\n",
    //     schedule_line, schedule_line + base_time, dumpInstPreg(inst));
    // }
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
  uint64_t sl_fu = getSLFU(inst);

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
      schedule_line = std::max({schedule_line, sl_mdp});
       DPRINTF(Cva6Sched, "Schedule (ADDR PRED    ): line %d T %d for %s\n",
        schedule_line, schedule_line + base_time, dumpInstPreg(inst));
      inst->vp_data.addr_taken = true; /* Mark taken */
    } else {
      schedule_line = std::max({schedule_line, sl_rr, sl_mdp});
    }
  } else if (!inst->isFault() && inst->staticInst->isStore()) {
    schedule_line = std::max({schedule_line, sl_rr, sl_st, sl_mdp});
  } else {
    schedule_line = std::max({schedule_line, sl_rr, sl_fu});
  }

  /* Fix the schedule line to avoid multiple load */
  if (!inst->isFault() && inst->staticInst->isLoad()){
    while (schedule_line < s2d.size() &&
      s2d[schedule_line].load_pushed >= loadPerCycle ){
        schedule_line ++;
      }
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
  /* FIrst of all: annotate reg mapping */
  for (auto reg: inst->regs_dst_phy){
    assert(reg.isRenammed);
    physical2arch[reg] = reg.virt_reg_idx;
  }
  inflight_insts_count += 1;
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


  /* Test a funky serialisation */
  // if (!s2d[schedule_line].canPush()){
  //   last_serialisation_time = base_time + schedule_line;
  // }

  /* Mark instruction scheduled line */
  inst->scheduled_time = base_time + schedule_line;

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

  /* Setup decoupled constraints */
  mla.onSchedule(inst, base_time + schedule_line);
  fumodel.onSchedule(inst, base_time + schedule_line);
  // mldabb.onSchedule(inst, base_time + schedule_line);
  // mlabb.onSchedule(inst, base_time + schedule_line);
  tick();
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
  /* Pop entry in scheduler */
  Cva6DynInstPtr inst = pop_front();
  tick();
  inflight_insts_count -= 1;
  DPRINTF(Cva6Sched, "SCHEDPOP: %s\n", dumpInstPreg(inst));
  return inst;
}

bool
SchedulerPierreMichaud::canRenameDest(Cva6DynInstPtr &inst,
  std::deque<uint64_t> &FL, uint64_t &preg) {
  bool hit = false;


  // PhysicalReg& reg = inst->regs_dst_phy[0];
  // uint64_t arch_reg_idx = reg.virt_reg_idx;

  uint64_t max_schedule_time = 0;
  uint64_t delta;
  uint64_t schedule_line = getScheduleLine(inst, delta);
  uint64_t schedule_time = schedule_line + base_time;
  // DPRINTF(Cva6Sched, "canRenameDest line %d T %d : %s\n",
  //   schedule_line, schedule_time, dumpInstPreg(inst));
  for (uint64_t pregi: FL){
    // bool same_mapping = physical2arch[pregi] == arch_reg_idx;
    // bool inFF = cpu.pipeline->sa.regalloc.isInFF(pregi);

    uint64_t preg_use_time = maxtimeoflasttouch[pregi];
    bool invalid = (preg_use_time > schedule_time); /* Suboptimal schedule */

    // (inFF && !same_mapping); // Avoid FF forwarding

    // DPRINTF(Cva6Sched, "Try preg : %d %s : T=%d [inFF=%d,%d]\n",
    //   pregi, invalid ? ".": "HIT", preg_use_time, inFF, same_mapping);

    if (invalid){
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
