#include "cpu/cva6/preschedulers/schedulervp.hh"

namespace gem5 {
namespace cva6 {

uint64_t
SchedulerVP::getScheduleLine(Cva6DynInstPtr inst, uint64_t& ready_slice) {
  uint64_t schedule_line = 0; // Active line
  ready_slice = cur_slice_idx; // by default use cur slice

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

  /* Register schedule line */
  for (auto &reg: inst->regs_src_phy){
    uint64_t sli = getSourceUseLine(reg);
    DPRINTF(Cva6Sched, "Schedule (RR: %s): line %d T %d for %s\n",
      reg, sli, sli + base_time, dumpInstPreg(inst));
    schedule_line = std::max(schedule_line, sli);
  }

  /* Store serialisation */
  schedule_line = std::max(schedule_line, getSLSTORE(inst));

  /* Register slice */
  bool init = false;
  uint64_t ready_slice_delay = getSliceUseLine(ready_slice);
  for (auto &reg: inst->regs_src_phy){
    /* get slice */
    uint64_t slice = sliceofreg[reg];
    /* Get slice delay */
    uint64_t slice_delay = getSliceUseLine(slice);
    // Pick the slice that is the deepest
    if (!init || slice_delay > ready_slice_delay){
      ready_slice = slice;
      ready_slice_delay = slice_delay;
      init = true;
    }
  }
  /* Final schedule line */
  schedule_line = std::max(schedule_line, ready_slice_delay);
  return schedule_line;
}
/* Scheduler VP*/
void SchedulerVP::push(Cva6DynInstPtr inst) {
  inflight_insts_count ++;

  uint64_t ready_slice;
  uint64_t schedule_line = getScheduleLine(inst, ready_slice);

  DPRINTF(Cva6Sched, "Schedule line %d T %d (slice=%d) for %s\n",
      schedule_line, schedule_line + base_time,
      ready_slice, dumpInstPreg(inst));

  /* Insert instruction */
  if (schedule_line >= s2d.size()){
    s2d.resize(schedule_line + 1);
  }
  assert(s2d[schedule_line].canPush());
  s2d[schedule_line].push(inst);
  /* Mark ready line */
  for (auto &reg: inst->regs_dst_phy){
    timeofregready[reg] = base_time + schedule_line + 1;
    sliceofreg[reg] = cur_slice_idx;
  }
  /* mark ready slice */
  time_of_slice_ready[ready_slice] = base_time + schedule_line + 1;

  /* Mark store */
  if (!inst->isFault() && inst->staticInst->isStore()){
    last_store_time = base_time + schedule_line;
  }

  inst->vp_data.value_taken = false;
  if (inst->vp_data.value_ready){
    assert(inst->staticInst);
    assert(inst->staticInst->isLoad());
    uint64_t value = inst->vp_data.pred_val;
    PhysicalReg &reg = inst->regs_dst_phy[0];
    /* Clear read dep */
    timeofregready[reg] = last_serialisation_time; // Serialisation time ??
    /* We also have to write predicted value to the PRF */
    cpu.pipeline->iq.forwardSpeculativeRegVal(reg, value);
    /* Mark the prediction taken */
    inst->vp_data.value_taken = true;
    DPRINTF(Cva6Sched, "SCHEDVP : UNLOCK %s with %lx\n",
      reg, value);
    // Increment cur slice
    // Compute next slice
    auto min = std::min_element(time_of_slice_ready.begin(),
      time_of_slice_ready.end());
    cur_slice_idx = std::distance(time_of_slice_ready.begin(), min);
    // cur_slice_idx = (cur_slice_idx + 1) % width;
    // And associate the register to this slice
    sliceofreg[reg] = cur_slice_idx;
  }
}

} // namespace cva6
} // namespace gem5
