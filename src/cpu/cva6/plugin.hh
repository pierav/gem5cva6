/**
 * plugin.cc
 *
 *  Author:   Pierre Ravenel
 * Created:   04/11/2023
 **/


#pragma once

#include <string>
#include <vector>

#include "base/named.hh"
#include "base/statistics.hh"
#include "base/time.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/pure_block.hh"
#include "debug/Cva6Plugin.hh"
#include "debug/Cva6Sched.hh"
#include "debug/Cva6SchedSched.hh"
#include "mem/packet.hh"

namespace gem5 {
namespace cva6 {

class Plugin : public Named
{
  protected:
    Cva6CPU &cpu;

  public:
    Plugin(const std::string &name, Cva6CPU &cpu_)
        : Named(name), cpu(cpu_) { ; }

    virtual void commit(Cva6DynInstPtr inst) = 0;
};

class PluginMemtrace : public Plugin
{
  protected:
    std::ofstream file;
  public:
    PluginMemtrace(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Plugin(name, cpu_)
    {
      init(params.plugin_memtrace_path);
    }
    void init(const std::string& path);
    void commit(Cva6DynInstPtr inst);

};

class PluginSimpointBar : public Plugin
{
  protected:
    size_t cpt = 0;
    size_t simcpt_size;
    bool is_enable = false;

    Cycles oldcycle;
    Cycles firstcycle;

    Time startTime;
    double oldtime;

  public:
    PluginSimpointBar(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Plugin(name, cpu_),
      oldcycle(0),
      firstcycle(0),
      startTime(true),
      oldtime(0.f)
    { init(params); }
    void init(const BaseCva6CPUParams &params);
    void commit(Cva6DynInstPtr inst);

};

class PluginVPP : public Plugin
{
  protected:
    VP &vp; /* Value predictor */
  public:
    PluginVPP(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Plugin(name, cpu_),
      vp(*vpinit(params.vpType, cpu.name() + ".vpp", cpu, params.vpSize))
    {}
    void commit(Cva6DynInstPtr inst);
};

class PluginMCVP : public Plugin
{
  protected:
    struct MCVPStats : public statistics::Group
    {
      /** Stats */
      statistics::Scalar none; // No one hit
      statistics::Scalar mc; // only mc hit
      statistics::Scalar vp; // only vp hit
      statistics::Scalar mcvp; // Both mc and vp hit

      statistics::Scalar mcnvpvpp; // MC ^ !vp ^ vpperfect
      statistics::Scalar nvpvpp; // !MC ^ !vp ^ vpperfect

      MCVPStats(BaseCPU &cpu) :
        statistics::Group(&cpu, "mcvp"),
        ADD_STAT(none, statistics::units::Count::get(),"none"),
        ADD_STAT(mc, statistics::units::Count::get(),"mc"),
        ADD_STAT(vp, statistics::units::Count::get(),"vp"),
        ADD_STAT(mcvp, statistics::units::Count::get(),"mcvp"),
        ADD_STAT(mcnvpvpp, statistics::units::Count::get(),"mcnvpvpp"),
        ADD_STAT(nvpvpp, statistics::units::Count::get(),"nvpvpp") { }
    } stats;
  public:
    PluginMCVP(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Plugin(name, cpu_), stats(cpu_) { }
    void commit(Cva6DynInstPtr inst);

};

#include "sim/sim_exit.hh"

class PluginGoodbadTrap : public Plugin
{
  protected:
    /** Termination */
    Addr passAddr;
    Addr failAddr;

  public:
    PluginGoodbadTrap(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Plugin(name, cpu_),
      passAddr(params.passAddr),
      failAddr(params.failAddr) { }
    void commit(Cva6DynInstPtr inst);

};


class PluginLambda : public Plugin
{
  protected:
    size_t n = 0;
    PureBlock handler;

  public:
    PluginLambda(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Plugin(name, cpu_),
      handler("lambda", cpu_, params) {
      // get a callback when we exit
      // registerExitCallback([this]() { handler.dump(); });
    }

  void commit(Cva6DynInstPtr inst){
    handler.commit(inst);
    n++;
    if (n % 1000000 == 0){
      // handler.dump();
    }
  }
};


#define PMC_RANGE 20

class PluginMemConst : public Plugin
{
  protected:
    std::map<uint64_t, std::map<uint64_t, uint64_t>> mem[PMC_RANGE];
    // PPN -> <Addr, Valid> -> Valid
    struct MemStats : public statistics::Group
    {
      /** Stats */
      statistics::Scalar req;
      statistics::Scalar reqll;
      statistics::Scalar reqsl;

      statistics::Vector reqllrange;

      MemStats(BaseCPU &cpu) :
        statistics::Group(&cpu, "memc"),
        ADD_STAT(req, statistics::units::Count::get(),"req"),
        ADD_STAT(reqll, statistics::units::Count::get(),"reqll"),
        ADD_STAT(reqsl, statistics::units::Count::get(),"reqsl"),
        ADD_STAT(reqllrange,statistics::units::Count::get(),"")
      {
        reqllrange.init(PMC_RANGE);
      }
    } stats;

  public:
    PluginMemConst(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Plugin(name, cpu_), stats(cpu_) { }
    void commit(Cva6DynInstPtr inst);

};

#define NB_INFLIGHTS 1000
#define SCHED_IGNORE_BRANCH 1
#define NBBBQ 32

inline std::string dumpInstPreg(Cva6DynInstPtr& inst){
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
        os << reg.str() << ", ";
      }
      os << " <- ";
      for (auto& reg: inst->regs_src_phy){
        os << reg.str() << ", ";
      }
      os << "]";
    }
  }
  return os.str();
}

template <class T>
class PhysicalRegFile
{
  class PhysicalRegHash_t
  {
  public:
    size_t operator()(const PhysicalReg &p) const {
      return p.isRenammed ? p.phys_reg_idx : -p.virt_reg_idx;
    }
  };

  std::unordered_map<PhysicalReg, T, PhysicalRegHash_t> map;
  public:
  PhysicalRegFile() {}
  T& operator[](PhysicalReg reg){
    assert(reg.classValue);
    return map[reg];
  }
};

class PhysicalRegAllocator
{
  std::deque<uint64_t> free_list;
  public:
  PhysicalRegAllocator(uint64_t nb_regs){
    for (int i = 0; i < nb_regs; i++){
      free_list.push_back(i);
    }
  }

  bool can_alloc(){
    return free_list.size();
  }

  uint64_t reg_alloc() {
    assert(free_list.size());
    uint64_t ret = free_list.front();
    free_list.pop_front();
    return ret;
  }

  void reg_free(uint64_t pidx){
    free_list.push_back(pidx);
  }

};



inline bool isBBend(Cva6DynInstPtr& inst){
  return !inst->isFault() && inst->staticInst->isControl();
}

struct waitqueue_t
{
  std::deque<Cva6DynInstPtr> holdqueue;

  PhysicalRegFile<unsigned int> holdregs;

  PhysicalRegAllocator &regalloc; /* Pointer back to the reg allocator */

  waitqueue_t(Cva6CPU &cpu, PhysicalRegAllocator &regalloc_)
    : regalloc(regalloc_) {}

  void push(Cva6DynInstPtr inst){
    /* Rename instruction : default is no renamming */
    BinaryRegisterFile rf;
    for (uint8_t i = 0; i < inst->numSrcRegs(); i++) {
      RegId regid = inst->srcRegIdx(i);
      if ((regid.classValue() != InvalidRegClass) && !rf.isSet(regid)){
        PhysicalReg reg(id2i(regid));
        reg.is_reg_dead = inst->exec_data.is_reg_dead[i];
        inst->regs_src_phy.push_back(reg);
        rf.set(regid);
      }
    }
    rf.clear();
    for (uint8_t i = 0; i < inst->numDstRegs(); i++) {
      RegId regid = inst->dstRegIdx(i);
      if ((regid.classValue() != InvalidRegClass) && !rf.isSet(regid)){
        inst->regs_dst_phy.push_back(PhysicalReg(id2i(regid)));
        rf.set(regid);
      }
    }
    /* Annotate missing Reg Dead */
    for (auto& reg: inst->regs_src_phy){
      if (rf.isSetRaw(reg.virt_reg_idx)){ /* Rf contains rd regs */
        reg.is_reg_dead = true;
      }
    }


    /* Try to rename to flying registers */
    /* Must be done before inserting instruction in the queue */
    tryRenameInst(inst);

    /* finally insert instruction in queue */
    holdqueue.push_back(inst);
  }

  bool isW(PhysicalReg &reg){
    for (auto it = holdqueue.rbegin(); it != holdqueue.rend(); ++it){
      for (PhysicalReg &ireg: (*it)->regs_dst_phy){
        if (ireg.virt_reg_idx == reg.virt_reg_idx){
          return true;
        }
      }
    }
    return false;
  }

  void tryRenameInst(Cva6DynInstPtr inst){
    if (inst->isFault()){
      return;
    }
    /* Try to rename all src registers */
    for (PhysicalReg &reg: inst->regs_src_phy) {
      if (!reg.is_reg_dead || /* Is a reg dead */
          !isW(reg) || /* Is the producer in the BB before */
          !regalloc.can_alloc() /* Is free physical reg */
      ){ continue; }
      // std::cout << "*** Rename reg " << reg.str()
      // << " on " << dumpInstPreg(inst) << "\n";
      assert(!reg.isRenammed);
      /* Allocate physical register */
      uint64_t pidx = regalloc.reg_alloc();
       /* Also rename inst reg */
      bool isrenamed = reg.doRenameIfMatchVreg(reg, pidx);
      assert(isrenamed);
      reg.isLastRename = true; /* Also mark the register deallocated */
      /* Rename regs on path */
      for (auto it = holdqueue.rbegin(); it!= holdqueue.rend(); ++it){
        // std::cout << "* try Rename " << dumpInstPreg(*it) << "\n";
        bool reach_writter = false;
        /* Leave condition */
        for (PhysicalReg &ireg: (*it)->regs_dst_phy){
          reach_writter |= ireg.doRenameIfMatchVreg(reg, pidx);
        }
        if (reach_writter){
          break; // Reack end
        }
        for (PhysicalReg &ireg: (*it)->regs_src_phy){
          ireg.doRenameIfMatchVreg(reg, pidx);
        }
      }
    }
  }

  Cva6DynInstPtr front(){
    return holdqueue.front();
  }

  Cva6DynInstPtr pop(){
    Cva6DynInstPtr inst = holdqueue.front();
    holdqueue.pop_front();

    /* Free allocated registers */
    if (!inst->isFault()){
      for (PhysicalReg& reg: inst->regs_src_phy){
        if (reg.isLastRename){ /* Is responsible for an allocation */
          assert(reg.isRenammed);
          regalloc.reg_free(reg.phys_reg_idx);
        }
      }
    }
    return inst;
  }

  // bool isRWaW(Cva6DynInstPtr inst){
  //   if (inst->isFault()){
  //     return false;
  //   }
  //   for (uint8_t i = 0; i < inst->numSrcRegs(); i++) {
  //     if (inst->srcRegIdx(i).classValue() != InvalidRegClass){
  //       if (holdregs[inst->srcRegIdx(i)]){
  //         return true;
  //       }
  //     }
  //   }
  //   return false;
  // }

  // bool isWaW(Cva6DynInstPtr inst){
  //   if (inst->isFault()){
  //     return false;
  //   }
  //   for (uint8_t i = 0; i < inst->numDstRegs(); i++) {
  //     if (inst->dstRegIdx(i).classValue() != InvalidRegClass){
  //       if (holdregs[inst->dstRegIdx(i)]){
  //         return true;
  //       }
  //     }
  //   }
  //   return false;
  // }
  template<class T>
  bool intersect(std::vector<T>& v0, std::vector<T>& v1){
    for (const T& x0: v0){
      for (const T& x1: v1){
        if (x0 == x1){
          return true;
        }
      }
    }
    return false;
  }
  /* Returns if instructions have dependancy with the queue */
  bool isDependancy(Cva6DynInstPtr inst){
    for (Cva6DynInstPtr &i2: holdqueue){
      /* Check registers dependancies */
      if (SCHED_IGNORE_BRANCH){
        if (isBBend(inst) || isBBend(i2)){
          continue;
        }
      }
      if (intersect(inst->regs_dst_phy, i2->regs_src_phy) /* WaR*/
       ||intersect(inst->regs_dst_phy, i2->regs_dst_phy) /* WaW*/
       ||intersect(inst->regs_src_phy, i2->regs_dst_phy) /* RaW */
      ){
        return true;
      }
      /* TODO: Check memory dependancies */
    }
    return false;
  }

  bool empty(){
    return holdqueue.empty();
  }

};

class StreamAnalyser
{
  public:
  uint64_t subclock = 0;
  uint64_t clock = 0;
  PhysicalRegFile<uint64_t> reglive;
  bool ignorebranch;
  struct Stats : public statistics::Group
  {
    statistics::Scalar stall;
    Stats(BaseCPU &cpu, const char *name) :
      statistics::Group(&cpu, name), ADD_STAT(stall, "") { }
  } stats;

  StreamAnalyser(BaseCPU &cpu, const char *name, bool ignorebranch_)
    : ignorebranch(ignorebranch_), stats(cpu, name) {}

  static uint64_t instructioncoststatic(Cva6DynInstPtr inst){
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

  uint64_t getInstReadyDeltaTime(Cva6DynInstPtr inst){
    if (inst->isFault()){
      return 0;
    }
    if (ignorebranch && isBBend(inst)){
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
    if (delta_time == 0){
      subclock += 1;
      const int issueWidth = 4;
      if (subclock == issueWidth){
        clock += 1;
        subclock = 0;
      }
    } else {
      clock += delta_time;
      subclock = 0;
    }
    stats.stall = clock;
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


class PluginScheduler : public Plugin
{
  protected:
    struct Stats : public statistics::Group
    {
      /** Stats */
      statistics::Scalar stallbase;
      statistics::Scalar stallsched;

      statistics::Scalar req;
      statistics::Scalar rescheduled;

      Stats(Cva6CPU &cpu) :
        statistics::Group(&cpu, "sched"),
        ADD_STAT(stallbase, ""),
        ADD_STAT(stallsched, ""),
        ADD_STAT(req, ""),
        ADD_STAT(rescheduled, "")
      { }
    } stats;

    /* Statistics only */
    uint64_t bbcnt = 0;
    StreamAnalyser instats;
    StreamAnalyser instatsnobr;
    StreamAnalyser outstats;
    std::deque<Cva6DynInstPtr> fifo;

    /* Renamming */
    PhysicalRegAllocator regalloc;

    /* Scheduler onternals */
    std::deque<waitqueue_t*> bbq;
    std::deque<Cva6DynInstPtr> tempoq;
    bool wait_drain = false;
    StreamAnalyser mainsa;

  public:
    PluginScheduler(const std::string &name,
      Cva6CPU &cpu,
      const BaseCva6CPUParams &params) :
      Plugin(name, cpu),
      stats(cpu),
      instats(cpu, "stream.in", false),
      instatsnobr(cpu, "stream.innobr", true),
      outstats(cpu, "stream.out", SCHED_IGNORE_BRANCH),
      regalloc(64),
      mainsa(cpu, "stream.sa", SCHED_IGNORE_BRANCH)
    {
      for (int i = 0; i < NBBBQ; i++){
        bbq.push_back(new waitqueue_t(cpu, regalloc));
      }
    }

    /* Insert instruction in scheduler */
    void push(Cva6DynInstPtr inst){
      // Time before instruction ready
      // uint64_t delta_time = instats.getInstReadyDeltaTime(inst);
      tempoq.push_back(inst);
      if (wait_drain && bbq.front()->empty()){ // Drain resolved, fill new slot
        bbq.push_back(bbq.front()); // Move last one to cur
        bbq.pop_front();
        wait_drain = false;
        bbcnt += 1;
      }
      /* Try to fill BB with temp data */
      while (!tempoq.empty() && !wait_drain){
        inst = tempoq.front();
        tempoq.pop_front();
        inst->bb_idx = bbcnt;
        wait_drain = !inst->isFault() && inst->staticInst->isControl();
        // DPRINTF(Cva6Sched, "Push %s\n", *inst);
        bbq.back()->push(inst);
      }
    }

    /* Retire instruction from scheduler */
    Cva6DynInstPtr pop(){
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

    void commit(Cva6DynInstPtr inst){
      fifo.push_back(inst);
      push(inst); // Push Inst in scheduler
      if (fifo.size() > NB_INFLIGHTS){
        Cva6DynInstPtr unschedisnt = fifo.front();
        fifo.pop_front();
        Cva6DynInstPtr schedinst = pop(); // Pop inst from scheduler
        uint64_t delta = mainsa.commit(schedinst);

        DPRINTF(Cva6SchedSched, "Schedule: %s (+%d)\n",
          dumpInstPreg(schedinst), delta);

        instats.commit(unschedisnt);
        instatsnobr.commit(unschedisnt);
        outstats.commit(schedinst);
      }
    }
};


} // namespace cva6
} // namespace gem5

