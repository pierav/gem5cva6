/**
 * scheduler.cc
 *
 *  Author:   Pierre Ravenel
 * Created:   12/11/2024
 **/

#include "cpu/cva6/scheduler.hh"

namespace gem5 {
namespace cva6 {

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
    os << "0x" << std::hex << inst->pc->instAddr() << std::dec << ": ";
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

void
SchedulerBB::push(Cva6DynInstPtr inst) {
  tempoq.push_back(inst);
  if (wait_drain && bbq.front()->empty()){ // Drain resolved, fill new slot
    bbq.push_back(bbq.front()); // Move last one to cur
    bbq.pop_front();
    wait_drain = false;
  }
  /* Try to fill BB with temp data */
  while (!tempoq.empty() && !wait_drain){
    inst = tempoq.front();
    tempoq.pop_front();
    wait_drain = !inst->isFault() && inst->staticInst->isControl();
    // DPRINTF(Cva6Sched, "Push %s\n", *inst);
    bbq.back()->push(inst);
  }
}

Cva6DynInstPtr
SchedulerBB::pop() {
  Cva6DynInstPtr ret = internal_pop();
  /* Annotate instruction delta */
  // TODO
  ret->delta = mainsa.commit(ret);
  return ret;
}

Cva6DynInstPtr
SchedulerBB::internal_pop(){
  /* Try OoO schedule */
  /* Compute instruction ready in respect to WaW deps */
  bool ready_to_schedule[NBBBQ];
  for (int i = 0; i < NBBBQ; i++){
    ready_to_schedule[i] = false;
    if (bbq[i]->empty()){
      continue;
    }
    Cva6DynInstPtr inst = bbq[i]->front();
    ready_to_schedule[i] = true;
    for (int j = 0; j < i; j++){
      if (bbq[j]->isDependancy(inst)){
        ready_to_schedule[i] = false;
        break;
      }
    }
  }
  int ready_to_schedule_cnt = 0;
  for (int i = 0; i< NBBBQ; i++){
    ready_to_schedule_cnt += ready_to_schedule[i];
  }

  uint64_t deltas[NBBBQ];
  for (int i = 0; i < NBBBQ; i++){
    if (!bbq[i]->empty()){
      deltas[i] = mainsa.getInstReadyDeltaTime(bbq[i]->front());
    }
  }

  assert(ready_to_schedule_cnt);
  DPRINTF(Cva6Sched, "============= schedpop() =============\n");
  if (ready_to_schedule_cnt){ /* Issue instruction with min Latency */
    int best_delta_idx = 0;
    int best_delta = 1000000; // Huge score
    for (int i = NBBBQ-1; i >= 0; i--){
      // Backward to prioritize recent instructions
      if (ready_to_schedule[i] && (deltas[i] <= best_delta)){
        best_delta_idx = i;
        best_delta = deltas[i];
      }
    }

    /* DUMP */
    for (int i = 0; i < NBBBQ; i++){
      DPRINTF(Cva6Sched, "BBQ[%d](ready=%d, delta=%d) :: [%s]\n",
        i, ready_to_schedule[i], deltas[i],
        i == best_delta_idx ? "HIT" : "");
      for (int j = 0; j < bbq[i]->holdqueue.size(); j++){
        DPRINTF(Cva6Sched, "BBQ[%d](%d): %s\n", i, j,
          dumpInstPreg(bbq[i]->holdqueue[j]));
      }
    }

    stats.req += 1;
    stats.rescheduled += best_delta_idx != 0;
    return bbq[best_delta_idx]->pop();
  } else { /* Issue pending instruction */
    assert(!tempoq.empty());
    auto ret = tempoq.front();
    tempoq.pop_front();
    return ret;
  }
}

uint64_t
SchedulerPierreMichaud::getSourceUseLine(PhysicalReg &reg){
  int64_t schedule_line = s2d.size()-1;
  for (; schedule_line >= 0; schedule_line--){
    scheduler_entry_t& se = s2d[schedule_line];
    if (se.isRaW(reg)){
      return se.execution_latency(reg) + schedule_line;
    }
  }
  return 0; // Active line
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
SchedulerPierreMichaud::getScheduleLine(Cva6DynInstPtr inst){
  uint64_t schedule_line = 0; // Active line
  for (auto &reg: inst->regs_src_phy){
    schedule_line = std::max(schedule_line, getSourceUseLine(reg));
  }
  DPRINTF(Cva6Sched, "Schedule : line %d for %s\n",
    schedule_line, dumpInstPreg(inst));

  uint64_t addr_load;
  if (isMemLoad(inst, addr_load)){
    /* MDP */
    Cva6DynInstPtr store_inst = Cva6DynInst::bubble();;
    bool is_dep = mdp.checkInst(inst->pc->instAddr(), &store_inst);
    uint64_t mdp_sched_line = is_dep ? find_inst_line(store_inst) : 0;

    /* Ideal MDP */
    Cva6DynInstPtr real_store_inst = Cva6DynInst::bubble();
    uint64_t ideal_mdp_sched_line = getScheduleLineForLoadAddr(addr_load,
      &real_store_inst);
    stats.mdp_false_positive += mdp_sched_line > ideal_mdp_sched_line;
    stats.mdp_true_positive += (mdp_sched_line == ideal_mdp_sched_line)
      && is_dep;
    stats.mdp_true_negative += (mdp_sched_line == ideal_mdp_sched_line)
      && !is_dep;
    stats.mdp_false_negative += mdp_sched_line < ideal_mdp_sched_line;
    DPRINTF(Cva6Sched, " | REAL memRaW line %d : %s\n",
      ideal_mdp_sched_line, dumpInstPreg(real_store_inst));
    DPRINTF(Cva6Sched, " | MDP predict line %d : %s\n",
          mdp_sched_line, dumpInstPreg(store_inst));

    /* Anomaly ! */
    if (mdp_sched_line < ideal_mdp_sched_line){
      assert(!real_store_inst->isBubble());
      // TODO reverse
      mdp.violation(real_store_inst->pc->instAddr(), inst->pc->instAddr());
    }
    /* Schedule with ideal */
    stats.load_bypass_store += 0; // TODO
    schedule_line = std::max(schedule_line, ideal_mdp_sched_line);
  }
  return schedule_line;
}

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

void
SchedulerPierreMichaud::push(Cva6DynInstPtr inst) {
  inflight_insts_count += 1;
  /* Mdp things */
  uint64_t addr;
  if (isMemWrite(inst, addr)){
    mdp.pushStore(inst->pc->instAddr(), inst);
  }

  uint64_t schedule_line = getScheduleLine(inst);
  /* Insert instruction */
  if (schedule_line >= s2d.size()){
    s2d.resize(schedule_line + 1);
  }
  s2d[schedule_line].push(inst, latency(inst));
}

Cva6DynInstPtr
SchedulerPierreMichaud::pop() {
  inflight_insts_count -= 1;
  /* Pop entry in scheduler */
  assert(s2d.size());
  scheduler_entry_t &se = s2d.front();
  assert(!se.empty());
  Cva6DynInstPtr inst = se.pop();
  /* Fix scheduler ring buffer */
  while (s2d.front().empty()){ // active_line ++ : Drop SE if clearred
    s2d.pop_front();
  }
  /* Mdp things */
  uint64_t addr;
  if (isMemWrite(inst, addr)){
    mdp.popStore(inst->pc->instAddr(), inst);
  }
  return inst;
}

}
}
