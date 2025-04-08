#pragma once

#include "cpu/cva6/pipeline.hh"
#include "cpu/cva6/scheduler.hh"

namespace gem5 {
namespace cva6 {

/**
 * VP based scheduling
 */
class SchedulerVP : public BaseScheduler
{
  struct Stats : public statistics::Group
  {
    statistics::Scalar req;
    statistics::Scalar rescheduled;

    Stats(Cva6CPU &cpu) :
      statistics::Group(&cpu, "SchedulerVP"),
      ADD_STAT(req, ""),
      ADD_STAT(rescheduled, "")
    { }
  } stats;

  /* Scheduler onternals */
  Cva6CPU &cpu;
  uint64_t size;
  uint64_t width;

  uint64_t base_time = 0;
  uint64_t inflight_insts_count = 0;

  MinLineAnalyserV2 mla;
  uint64_t last_store_time = 0;
  uint64_t last_serialisation_time = 0;

  // struct cycle_entry_t {
  //   Cva6DynInstPtr insts[4] = { nullptr };
  // };
  std::deque<scheduler_entry_t> s2d;
  PhysicalRegFile<unsigned int> timeofregready;

  PhysicalRegFile<unsigned int> sliceofreg;
  std::vector<uint64_t> time_of_slice_ready;

  uint64_t cur_slice_idx = 0;

  public:
  SchedulerVP(const std::string &name,
              Cva6CPU &cpu_,
              const BaseCva6CPUParams &p) :
    stats(cpu_), cpu(cpu_), size(p.schedSize), width(p.schedWidth),
    mla(16) {
      scheduler_entry_t::width = p.schedWidth;
      time_of_slice_ready.resize(p.schedWidth);
    }
  /* Interface */
  bool canPush(Cva6DynInstPtr inst) override {
    return inflight_insts_count < size;
  }
  uint64_t getSourceUseLine(PhysicalReg &reg){
    uint64_t alt_line = timeofregready[reg] > base_time ?
                        timeofregready[reg] - base_time : 0;
    return alt_line;
  }

  uint64_t getSliceUseLine(uint64_t slice){
    uint64_t alt_line = time_of_slice_ready[slice] > base_time ?
                        time_of_slice_ready[slice] - base_time : 0;
    return alt_line;
  }

  uint64_t getSLSTORE(Cva6DynInstPtr &inst){
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

  uint64_t getScheduleLine(Cva6DynInstPtr inst, uint64_t& ready_slice);
  void push(Cva6DynInstPtr inst);

  Cva6DynInstPtr pop() override {
    inflight_insts_count --;
    /* Pop entry in scheduler */
    assert(s2d.size());
    scheduler_entry_t &se = s2d.front();
    assert(!se.empty());
    Cva6DynInstPtr inst = se.pop();
    while (!s2d.empty() && s2d.front().empty()){
      s2d.pop_front();
      base_time ++;
    }
    DPRINTF(Cva6Sched, "SCHEDPOP: %s\n", dumpInstPreg(inst));
    return inst;
  }
  Cva6DynInstPtr front() override {
    assert(inflight_insts_count);
    assert(s2d.size());
    assert(s2d.front().size());
    return s2d.front().front();
  };
  bool canPop() override { return inflight_insts_count;}
  void flush() override {
    inflight_insts_count = 0;
    /* Set base time to final time (clean debug)*/
    base_time += s2d.size();
    /* Clear everything */
    s2d.clear();
    timeofregready.setall(0);
    for (int i = 0; i < width; i++){
      time_of_slice_ready[i] = 0;
    }
    last_serialisation_time = 0;
    last_store_time = 0;
  }
};

} // namespace cva6
} // namespace gem5
