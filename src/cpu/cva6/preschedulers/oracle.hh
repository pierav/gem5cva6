/**
 * @file orache.hh
 * @author Pierre Ravenel (pravenel@kalrayinc.com)
 * @brief Scheduler Dataflow Oracle
 * @version 1.0
 * @date 2025-07-16
 */

#include "base/named.hh"
#include "base/statistics.hh"
#include "cpu/cva6/scheduler.hh"

namespace gem5 {
namespace cva6 {

#if 0
class OracleSchedList : public Named
{
    bool branch_ser;
    bool mem_ser;

    public:
    /* Constructor */
    OracleSchedList(
        const std::string &name,
        Cva6CPU &cpu, bool branch_ser_, bool mem_ser_
    ) : Named(name), branch_ser(branch_ser_), mem_ser(mem_ser_) {}

    std::deque<Cva6DynInstPtr> iq;

    uint64_t serialised_time = 0;
    uint64_t last_store_time = 0;
    SchedulerPierreMichaud::FUModel fumodel;
    uint64_t base_time = 0;
    ArchRegFile<unsigned int> timeofregready;
    std::map<uint64_t/* Addr */, uint64_t/* Count */> timeofaddrready;


    /* Base primitives */
    uint64_t regNotReady(PhysicalReg &reg){
        return timeofregready[reg] > base_time;
    }

    uint64_t addrNotReady(uint64_t addr){
        addr |= 0b111; // DW mask
        return timeofaddrready[addr] > base_time;
    }


    uint64_t getSLFU(Cva6DynInstPtr &inst){
      uint64_t sched_line = 0;
      uint64_t ready_time = fumodel.getRT(inst);
      if (ready_time > base_time){
        sched_line = ready_time - base_time;
        DPRINTF(Cva6Sched, "Schedule (SL FU        ): line %d T %d for %s\n",
            sched_line, ready_time, dumpInstPreg(inst));
      }
      return sched_line;
    }

    uint64_t isReady(Cva6DynInstPtr inst){
        for (auto &reg: inst->regs_src_phy){
            if (regNotReady(reg)){
              return false;
            }
        }
        uint64_t addr_load;
        if (isMemLoad(inst, addr_load)){ /* Ideal MDP */
          if (addrNotReady(addr)){
            return false;
          }
        }
        return true;
    }

    Cva6DynInstPtr internal_pop(){
      while (1){
        for (Cva6DynInstPtr inst: iq){
          if (isReady(inst)){
            iq.remove(inst);
            return inst;
          }
        }
        // If fail : increase time
        base_time ++;
      }
    }

    Cva6DynInstPtr pop(){
      Cva6DynInstPtr inst = internal_pop():
      if (isMemWrite(inst, addr)){
            timeofaddrready[addr | 0b111] = schedule_time;
        }
        if (!inst->isFault() && inst->staticInst->isMemRef()){
          last_store_time = schedule_time;
        }
        uint64_t ready_time = schedule_time + instructioncoststatic(inst);
        for (auto &reg: inst->regs_dst_phy){
            timeofregready[reg] = ready_time;
        }
        if (branch_ser){
          if (!inst->isFault() && inst->staticInst->isControl()){
            serialised_time = schedule_time;
          }
        }
        fumodel.onSchedule(inst, schedule_time);
    }
}

#endif

#define ISSUE_WIDTH_SCHED 128

class OracheSched : public Named
{
    bool branch_ser;
    bool mem_ser;

    public:

    bool isMemLoad(Cva6DynInstPtr& inst, uint64_t &paddr){
        if (!inst->isFault()
            && inst->staticInst->isLoad()){
            assert(inst->dreq);
            paddr = inst->dreq->getPaddr();
            return true;
        }
        return false;
    }

    bool isMemWrite(Cva6DynInstPtr& inst, uint64_t &paddr){
        if (!inst->isFault()
            && inst->staticInst->isMemRef()
            && !inst->staticInst->isLoad()){
            fatal_if(!inst->dreq, "Must have dreq: %s\n", *inst);
            paddr = inst->dreq->getPaddr();
            return true;
        }
        return false;
    }

    struct scheduler_entry_t
    {
        uint64_t pushed = 0;
        std::deque<Cva6DynInstPtr> slots;
        public:
        int width = ISSUE_WIDTH_SCHED;
        bool canPush(){ return pushed < width; }
        void push(Cva6DynInstPtr inst){
            pushed++;
            slots.push_back(inst);
        }

        Cva6DynInstPtr front(){ return slots.front(); }
        size_t size(){ return slots.size(); }

        Cva6DynInstPtr pop(){
            Cva6DynInstPtr inst = slots.front();
            slots.pop_front();
            return inst;
        }
        bool empty(){ return slots.empty(); }
    };

    uint64_t serialised_time = 0;
    uint64_t last_store_time = 0;
    SchedulerPierreMichaud::FUModel fumodel;
    uint64_t base_time = 0;
    ArchRegFile<unsigned int> timeofregready;
    std::map<uint64_t/* Addr */, uint64_t/* Count */> timeofaddrready;

    // MDP related
    std::map<uint64_t/* Addr */, Cva6DynInstPtr/* inst */> instofaddrready;
    StoreSet<Cva6DynInstPtr> mdp;       /* MDP predictor */

    /* The main containers */
    std::deque<scheduler_entry_t> s2d; // 2D array Scheduler

    struct Stats : public statistics::Group
    {
        statistics::Scalar req;
        statistics::Scalar rescheduled;
        statistics::Scalar load_bypass_store;
        statistics::Scalar mdp_false_positive;
        statistics::Scalar mdp_true_positive;
        statistics::Scalar mdp_true_negative;
        statistics::Scalar mdp_false_negative;

        Stats(Cva6CPU &cpu, std::string name) :
        statistics::Group(&cpu, (name + ".SchedulerOracle").c_str()),
        ADD_STAT(req, ""),
        ADD_STAT(rescheduled, ""),
        ADD_STAT(load_bypass_store, "A load bypassed a previous store"),
        ADD_STAT(mdp_false_positive, ""),
        ADD_STAT(mdp_true_positive, ""),
        ADD_STAT(mdp_true_negative, ""),
        ADD_STAT(mdp_false_negative, "")
        { }
    } stats;

    /* Constructor */
    OracheSched(
        const std::string &name,
        Cva6CPU &cpu, bool branch_ser_, bool mem_ser_
    ) : Named(name), branch_ser(branch_ser_), mem_ser(mem_ser_),
        mdp(2048), stats(cpu, name) {}

    uint64_t find_inst_line(Cva6DynInstPtr inst);

    /* Base primitives */
    uint64_t getSourceUseLine(PhysicalReg &reg){
        return timeofregready[reg] > base_time ?
            timeofregready[reg] - base_time : 0;
    }

    uint64_t getSLforAddr(uint64_t addr){
        addr |= 0b111; // DW mask
        return timeofaddrready[addr] > base_time ?
            timeofaddrready[addr] - base_time : 0;
    }


    uint64_t getSLFU(Cva6DynInstPtr &inst){
      uint64_t sched_line = 0;
      uint64_t ready_time = fumodel.getRT(inst);
      if (ready_time > base_time){
        sched_line = ready_time - base_time;
        DPRINTF(Cva6Sched, "Schedule (SL FU        ): line %d T %d for %s\n",
            sched_line, ready_time, dumpInstPreg(inst));
      }
      return sched_line;
    }

    uint64_t getScheduleLine(Cva6DynInstPtr inst){
        uint64_t schedule_line = 0; // The min line
        if (base_time < serialised_time){
          schedule_line = serialised_time - base_time;
        }
        schedule_line = std::max(schedule_line, getSLFU(inst));
        for (auto &reg: inst->regs_src_phy){
            schedule_line = std::max(schedule_line, getSourceUseLine(reg));
        }
        uint64_t addr_load;
        if (isMemLoad(inst, addr_load)){ /* Ideal MDP */
            if (mem_ser && last_store_time > base_time){
              uint64_t store_line = last_store_time - base_time;
              schedule_line = std::max(schedule_line, store_line);
            } else {
              uint64_t ideal_mdp_sched_line = getSLforAddr(addr_load);
              schedule_line = std::max(schedule_line, ideal_mdp_sched_line);
            }
        }
        return schedule_line;
    }

    void push(Cva6DynInstPtr inst) {
        uint64_t schedule_line = getScheduleLine(inst);
        DPRINTF(Cva6Sched, "Schedule : line %d for %s\n",
            schedule_line, inst);
        /* Ignore already filled lines */
        while (schedule_line < s2d.size() && !s2d[schedule_line].canPush()){
            schedule_line ++;
        }
        /* Insert instruction */
        if (schedule_line >= s2d.size()){
            s2d.resize(schedule_line + 1);
        }
        s2d[schedule_line].push(inst);

        // Mark reg and mem
        uint64_t schedule_time = base_time + schedule_line;
        uint64_t addr;
        if (isMemWrite(inst, addr)){
            timeofaddrready[addr | 0b111] = schedule_time;
            instofaddrready[addr | 0b111] = inst;
        }
        if (!inst->isFault() && inst->staticInst->isMemRef()){
          last_store_time = schedule_time;
        }
        uint64_t ready_time = schedule_time + instructioncoststatic(inst);
        for (auto &reg: inst->regs_dst_phy){
            timeofregready[reg] = ready_time;
        }
        if (branch_ser){
          if (!inst->isFault() && inst->staticInst->isControl()){
            serialised_time = schedule_time;
          }
        }
        fumodel.onSchedule(inst, schedule_time);
        inst->scheduled_time_oracle = schedule_time;

        /* Predict MDP */
        /* Predict memory dependancies */
        uint64_t addr_load;
        if (isMemLoad(inst, addr_load)){
            Cva6DynInstPtr mdpinst;
            bool is_dep = mdp.checkInst(inst->pc->instAddr(), &mdpinst);
            uint64_t s_time_ideal = timeofaddrready[addr_load | 0b111];
            if (s_time_ideal < base_time){
                s_time_ideal = 0;
            }
            uint64_t s_time_mdp = is_dep ? mdpinst->scheduled_time_oracle : 0;
            if (s_time_mdp < base_time){
                s_time_mdp = 0;
            }

            bool is_dep_eff = s_time_ideal != 0;
            bool is_valid = s_time_mdp >= s_time_ideal;
            // Valid schedule
            stats.mdp_false_positive += is_valid && is_dep && !is_dep_eff;
            stats.mdp_true_positive  += is_valid && is_dep && is_dep_eff;
            stats.mdp_true_negative  += is_valid && !is_dep;
            // Invalid schedule
            stats.mdp_false_negative += !is_valid;
            stats.req += 1;

            if (s_time_mdp < s_time_ideal){
                uint64_t pcself = inst->pc->instAddr();
                Cva6DynInstPtr violinst = instofaddrready[addr_load | 0b111];
                assert(violinst);
                uint64_t pcstore = violinst->pc->instAddr();
                mdp.violation(pcstore, pcself);
            }
        }
        /* mark store if needed. Predict before update. */
        if (!inst->isFault() && inst->staticInst->isStore()){
            mdp.pushStore(inst->pc->instAddr(), inst);
        }
    }

    Cva6DynInstPtr pop() {
        /* Pop entry in scheduler */
        assert(s2d.size());
        scheduler_entry_t &se = s2d.front();
        assert(!se.empty());
        Cva6DynInstPtr inst = se.pop();
        /* Fix scheduler ring buffer */
        while (s2d.front().empty()){
            s2d.pop_front();
            base_time++;
        }

        /* Mdp things */
        if (!inst->isFault() && inst->staticInst->isStore()){
            mdp.popStore(inst->pc->instAddr(), inst);
        }
        uint64_t addr;
        if (isMemWrite(inst, addr)){
            uint64_t idx = addr | 0b111;
            if (timeofaddrready[idx] <= base_time) {
              timeofaddrready.erase(idx);
              instofaddrready.erase(idx);
            }
        }
        return inst;
    }
};


class StreamAnalyserV2
{
  public:
  uint64_t subclock = 0;
  uint64_t clock = 0;
  PhysicalRegFile<uint64_t> reglive;
  struct Stats : public statistics::Group
  {
    statistics::Scalar stall;
    Stats(BaseCPU &cpu, const char *name) :
      statistics::Group(&cpu, name), ADD_STAT(stall, "") { }
  } stats;

  StreamAnalyserV2(BaseCPU &cpu, const char *name)
    : stats(cpu, name) {}

  uint64_t getInstReadyDeltaTime(Cva6DynInstPtr inst){
    if (inst->isFault()){
      return 0;
    }
    uint64_t ready_time = clock;
    for (const PhysicalReg& reg: inst->regs_src_phy){
      ready_time = std::max(reglive[reg], ready_time);
    }
    if (clock >= ready_time){
      return 0;
    } else {
      return ready_time - clock;
    }
  }

  uint64_t commit(Cva6DynInstPtr inst){
    /* Read register availability */
    uint64_t delta_time = getInstReadyDeltaTime(inst);
    /* Increase clock until instruction ready */
    uint64_t clock_before = clock;
    if (delta_time == 0){
      subclock += 1;
      if (subclock == ISSUE_WIDTH_SCHED){
        clock += 1;
        subclock = 1;
      }
    } else {
      clock += delta_time;
      subclock = 0;
    }
    /* Use clock derivation to handle stats reset */
    stats.stall += clock - clock_before;
    /* Set Rd nes ts */
    uint64_t delta_finish = instructioncoststatic(inst);
    uint64_t readytime = clock + delta_finish;
    if (!inst->isFault()){
      for (const PhysicalReg& reg: inst->regs_dst_phy){
        reglive[reg] = readytime;
      }
    }
    DPRINTF(Cva6Sched, "CLOCK(%d) += STALL(%d) :: -> +READY(%d):%d : %s\n",
      clock, delta_time, delta_finish, readytime, *inst);
    return delta_time;
  }
};

class OracleBench
{
    OracheSched sched;
    size_t size;
    StreamAnalyserV2 isa;
    StreamAnalyserV2 osa;
    std::deque<Cva6DynInstPtr> fifo;
    uint64_t cnt = 0;

    public:
    OracleBench(Cva6CPU& cpu, std::string name,
      uint64_t window_size, bool bser, bool mser):
        sched(name, cpu, bser, mser),
        size(window_size),
        isa(cpu, (name + "stream.in").c_str()),
        osa(cpu, (name + "stream.out").c_str()) {}

    void tick(Cva6DynInstPtr &inst){
        sched.push(inst);
        fifo.push_back(inst);
        cnt++;
        if (cnt > size){
            osa.commit(sched.pop());
            isa.commit(fifo.front());
            fifo.pop_front();
        }
    }
};


} // namespace cva6
} // namespace gem5
