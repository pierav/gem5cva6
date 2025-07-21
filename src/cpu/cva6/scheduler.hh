/**
 * scheduler.hh
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
#include "cpu/cva6/store_set.hh"
#include "debug/Cva6Rename.hh"
#include "debug/Cva6Sched.hh"
#include "debug/Cva6SchedSched.hh"

namespace gem5 {
namespace cva6 {

#define NB_INFLIGHTS 100
#define SCHED_IGNORE_BRANCH 1
#define NBBBQ 4

bool needSerialise(Cva6DynInstPtr inst);

std::string dumpInstPreg(Cva6DynInstPtr& inst);
uint64_t instructioncoststatic(Cva6DynInstPtr inst);

inline bool isBBend(Cva6DynInstPtr& inst){
  return !inst->isFault() && inst->staticInst->isControl();
}

// inline bool isMemWrite(Cva6DynInstPtr& inst, uint64_t &paddr){
//   if (!inst->isFault()
//      && inst->staticInst->isMemRef()
//      && !inst->staticInst->isLoad()){
//       fatal_if(!inst->dreq, "Must have dreq: %s\n", *inst);
//       paddr = inst->dreq->getPaddr();
//     return true;
//   }
//   return false;
// }

// inline bool isMemLoad(Cva6DynInstPtr& inst, uint64_t &paddr){
//   if (!inst->isFault()
//      && inst->staticInst->isLoad()){
//       assert(inst->dreq);
//       paddr = inst->dreq->getPaddr();
//     return true;
//   }
//   return false;
// }

#define PREG_MAGIC 10000

class PhysicalRegAllocator : public Named
{
  Cva6CPU &cpu;
  struct PhysicalRegAllocatorStats : public statistics::Group
  {
    statistics::Scalar reg_alloc;
    statistics::Scalar reg_free;
    statistics::Scalar reg_free_spec;
    statistics::Scalar reg_free_commit;
    PhysicalRegAllocatorStats(const std::string &name, BaseCPU &cpu):
    statistics::Group(&cpu, name.c_str()),
    ADD_STAT(reg_alloc, "Number of reg allocated"),
    ADD_STAT(reg_free, "Number of insts freed"),
    ADD_STAT(reg_free_spec, "Number of insts freed spec"),
    ADD_STAT(reg_free_commit, "Number of insts freed commit") {}
  } stats;

  size_t n;
  std::deque<uint64_t> free_list;
  std::deque<uint64_t> free_list_popped;

  ArchRegFile<uint64_t> rmt;
  ArchRegFile<uint64_t> rmt_valid;

  ArchRegFile<uint64_t> rmt_checkpoint;

  // Owner of the physical reg !!!
  PhysicalRegFile<uint64_t> rmt_owner;
  enum state_t
  {
    FREE_COMMIT,
    FREE_SPEC,
    BUZY
  };

  PhysicalRegFile<state_t> isbuzy;
  PhysicalRegFile<char> inFF;


  PhysicalRegFile<char> cannotbefreed;

  public:
  bool isInFF(uint64_t preg){
    return isbuzy[preg] == FREE_COMMIT && inFF[preg];
  }
  std::string dump(){
    std::ostringstream os;
    os << '[';
    if (incarchreg){
      for (auto x: free_list){
        os << x << " ";
      }
    } else {
      for (int i = 0; i < n; i++){
        switch (isbuzy[i]){
          case FREE_COMMIT:
            os << i << "C ";
            break;
          case FREE_SPEC:
            os << i << "S ";
            break;
          case BUZY:
            os << " . ";
            break;
        }
      }
    }
    os << ']';
    return os.str();
  }

  bool incarchreg; // Scoreboard arch or Full RR
  bool freeregdead;
  bool specrelease;
  bool specreleasePC;
  public:
  PhysicalRegAllocator(
    const std::string &name_,
    Cva6CPU &cpu_,
    const BaseCva6CPUParams &p,
    uint64_t nb_regs,
    bool incarchreg_,
    bool freeregdead_,
    bool specrelease_) :
    Named(name_),
    cpu(cpu_),
    stats(name_, cpu_),
    n(nb_regs), incarchreg(incarchreg_),
    freeregdead(freeregdead_),
    specrelease(specrelease_),
    specreleasePC(p.renameSpecReleasePC) {
    fatal_if(incarchreg && nb_regs <= NB_I2ID, "Need more preg");
    // Default RMT
    for (int i = 0; i < 64; i++){
      RegId regid = i2id(i);
      PhysicalReg reg(regid);
      rmt[reg] = PREG_MAGIC;
      rmt_valid[reg] = true;
    }
    /* Initialise all arch regs to physical mapping */
    for (int i = 0; i < nb_regs; i++){
      // Riscv : ignore zero reg
      if (incarchreg && i >= 1 && i < NB_I2ID){
        RegId regid = i2id(i);
        PhysicalReg reg(regid);
        reg.doRename(i);
        rmt[reg] = i;
        isbuzy[reg] = BUZY;
      } else { /* Let the register for future use */
        free_list.push_back(i);
      }
    }
    rmt_checkpoint = rmt; // Default rmt !
  }
  uint64_t size(){
    return n;
  }
  bool canRename(){
    if (!free_list.size()){
      // DPRINTF(Cva6Rename, "Out of PREG\n");
    }
    return free_list.size();
  }

  void rename_one_secure(PhysicalReg& reg){
    if (!reg.isRenammed){
      reg.doRename(rmt[reg]);
      reg.producer_id = rmt_owner[reg];
      reg.isRenammedValid = rmt_valid[reg];
    }
  }

  void rename_src(Cva6DynInstPtr& inst){
    /* Rename srcs */
    for (PhysicalReg& reg: inst->regs_src_phy){
      rename_one_secure(reg);
    }
  }

  std::deque<uint64_t>& getFL(){
    return free_list;
  }
  void rename_dst(Cva6DynInstPtr& inst, uint64_t preg){
    assert(inst->regs_dst_phy.size() == 1);
    PhysicalReg& reg = inst->regs_dst_phy.front();
    /* Keep track of old mapping for reg free */
    PhysicalReg freereg = reg; // Ok because rmt [ ArchReg ]
    rename_one_secure(freereg); // Clean rename of dst
    inst->phys_reg_to_free.push_back(freereg);

    /* > Invalidate RMT */
    // > If someone do not have allocated the same ArchReg !
    // if (rmt[reg] == reg.phys_reg_idx){
    //   rmt[reg] = PREG_MAGIC;
    // }
    // > Instead of this we may defer the RMT invalidation.
    // > This can be done at allocate. Do do this, it requires
    // > to know the old arch reg.
    // rmt_owner[reg] = 0; // Do not let think the owner own the rmt
    // Safe check: do not let multiple allocation
    // rmt.swap_value(preg, PREG_MAGIC);
    if (!incarchreg){
      for (int areg = 0; areg < rmt.size(); areg++){
        if (rmt[areg] == preg){
          rmt_valid[areg] = false; // Valid instead of magic
        }
      }
    }
    // for (uint64_t id: rmt){
    //   fatal_if(id == preg, "PREG %d is mapped in RMT\n", id);
    // }

    /* pop register from free list */
    fatal_if(!free_list.size(), "No more entry in FL\n");
    // OLD
    // uint64_t pregidx = free_list.front();
    // free_list.pop_front();
    // NEW : pop everywhere
    auto it = std::find(free_list.begin(), free_list.end(), preg);
    fatal_if(it==free_list.end(), "Preg %d must be in FL\n", preg);
    it = free_list.erase(it);
    if (incarchreg){
      free_list_popped.push_back(preg);
    }
    /* Mark many things */
    rmt[reg] = preg; /* 1) Update the RMT */
    rmt_valid[reg] = true;
    rmt_owner[preg] = inst->id.fetchSeqNum; /* 2) The owner */
    isbuzy[preg] = BUZY; /* 3) RMT or FREE list (must be exclusive)*/
    stats.reg_alloc += 1;
    /* Finally rename the register */
    rename_one_secure(reg);
    DPRINTF(Cva6Rename, "Alloc reg %s :: FL=[%s]\n", reg, dump());
  }
  private:

  // bool is_allocated(PhysicalReg& reg){
  //   return reg.phys_reg_idx != PREG_MAGIC &&
  //          isbuzy[reg] == BUZY &&
  //          rmt_owner[reg] == reg.producer_id;
  // }

  bool free_reg(PhysicalReg& reg, bool speculative=false){
    // std::cout << "Free " << idx << std::endl;
    if (reg.isRenammedValid){
      if (speculative && cannotbefreed[reg]){
        return false;
      }
      DPRINTF(Cva6Rename,
        "Free reg  %s :: FL=[%s] :: isbuzy[%s]=%d rmt_owner[%s]=%lx"
        "(prod=%lx)\n",
        reg, dump(), reg, isbuzy[reg], reg, rmt_owner[reg],
        reg.producer_id);
      // Safe check: do not release twice
      // Is the reg still in use and we are the owner
      if (isbuzy[reg] == BUZY &&
        (rmt_owner[reg] == reg.producer_id ||
         rmt_owner[reg] == 0)){
        cannotbefreed[reg] = false;
        isbuzy[reg] = speculative ? FREE_SPEC : FREE_COMMIT;
        for (uint64_t id: free_list){
          fatal_if(id == reg.phys_reg_idx, "PREG %d is already freed\n", id);
        }
        free_list.push_back(reg.phys_reg_idx);
        return true;
      }
    }
    return false;
  }

  public:
  void speculative_update(Cva6DynInstPtr& inst){
    if (!incarchreg){ // If no arch reg
      if (specrelease){
        /* Free registers WaW */
        for (PhysicalReg& reg: inst->phys_reg_to_free){
          stats.reg_free_spec += free_reg(reg, true);
        }
        /* Can we free RDeadaW ? */
        if (freeregdead){
          for (PhysicalReg& reg: inst->regs_src_phy){
            if (reg.is_reg_dead){ // TODO
              stats.reg_free_spec + free_reg(reg, true);
            }
          }
        }
      }
    }
  }

  void reg_barrier(){ // Move all speculative freed to BUZY
    // DPRINTF(Cva6Rename, "reg_barrier before %s\n", dump());
    // for (auto it = free_list.begin(); it != free_list.end();){
    //   uint64_t preg = *it;
    //   // if (isbuzy[preg] == FREE_SPEC){
    //   //   isbuzy[preg] = BUZY;
    //   //   it = free_list.erase(it);
    //   // } else {
    //   //   it++;
    //   // }
    //   if (isbuzy[preg] == BUZY){ // Park buzy reg
    //     cannotbefreed[preg] = true; //
    //   }
    // }
    // Safe check
    // for (state_t s: isbuzy){
    //   assert(s != FREE_SPEC);
    // }
    // DPRINTF(Cva6Rename, "reg_barrier after %s\n", dump());
    for (int preg = 0; preg < n; preg++){
      if (isbuzy[preg] == BUZY){ // Park buzy reg
        cannotbefreed[preg] = true; //
      }
    }
  }

  bool reg_available(PhysicalReg& reg){
    assert(reg.phys_reg_idx != PREG_MAGIC);
    // TODO use cannotbefreed ?
    return rmt_owner[reg] == reg.producer_id;
  }

  void pre_commit(Cva6DynInstPtr& inst){
    if (specreleasePC){
      _commit(inst);
    }
  }

  void commit(Cva6DynInstPtr& inst){
    if (!specreleasePC){
      _commit(inst);
    }
  }

  /* Free all registers */
  void _commit(Cva6DynInstPtr& inst){
    if (!incarchreg){ // If no arch reg
      /* free_rd_at_commit */
      for (PhysicalReg& reg: inst->regs_dst_phy){
        inst->free_reg_at_commit = free_reg(reg);
        stats.reg_free_commit += inst->free_reg_at_commit;
        if (inst->free_reg_at_commit){
          inFF[reg] = true;
        }
      }
      return;
    }
    /* Free registers WaW */
    for (PhysicalReg& reg: inst->phys_reg_to_free){
      stats.reg_free_commit += free_reg(reg);
    }
    /* Update checkpoint with new allocations */
    for (PhysicalReg& reg: inst->regs_dst_phy){
      /* Assume InO commit */
      // std::cout << "ALLOC " << reg << " old was:
      //  " << rmt_checkpoint[reg] << std::endl;
      fatal_if(free_list_popped.front() != reg.phys_reg_idx,
        "Bad Free must be %d, got %d (%s)\n", free_list_popped.front(),
        reg.phys_reg_idx, *inst);
      free_list_popped.pop_front();
      rmt_checkpoint[reg] = reg.phys_reg_idx;
    }
  }

  void post_commit(Cva6DynInstPtr& inst){
    if (!incarchreg){ // If no arch reg
      /* free_rd_at_commit */
      for (PhysicalReg& reg: inst->regs_dst_phy){
        if (inst->free_reg_at_commit){
          inFF[reg] = false;
        }
      }
      return;
    }
  }
  void flush(){
    /* Mv rmt_checkpoint to rmt */
    if (incarchreg){ // Restore a valid RMT
      rmt = rmt_checkpoint;
      /* Fix free list */
      /* [...|...|...]FL.front() <- FLP.back()[...|...] */
      while (free_list_popped.size()){ /* Both revsersed */
        free_list.push_front(free_list_popped.back());
        free_list_popped.pop_back();
      }
    } else {
      // Symply clear the rmt
      // rmt.setall(PREG_MAGIC);
      rmt_valid.setall(false);
      isbuzy.setall(FREE_COMMIT);
      inFF.setall(false);
      cannotbefreed.setall(false);
      // And reset FL
      free_list_popped.clear();
      free_list.clear();
      for (int i = 0; i < n; i++){
        free_list.push_back(i);
      }
    }
  }
};


class BaseScheduler
{
  public:
  virtual bool canPush(Cva6DynInstPtr inst) = 0;
  virtual void push(Cva6DynInstPtr inst) = 0;
  virtual bool canPop() = 0;
  virtual Cva6DynInstPtr front() = 0;
  virtual Cva6DynInstPtr pop() = 0;
  virtual void flush() = 0;
  virtual bool canRenameDest(Cva6DynInstPtr &inst,
    std::deque<uint64_t> &FL, uint64_t &preg){
    if (FL.size()){ // Rename to the first
      preg = FL.front();
      return true;
    }
    return false; /* Cannot rename */
  }
  virtual size_t size() {
    return 1;
  }
  virtual void tick(){} /* Tick the scheduler */
};

class NoScheduler : public BaseScheduler
{
  uint64_t size;
  std::deque<Cva6DynInstPtr> fifo;
  public:
  NoScheduler(const std::string &name,
      Cva6CPU &cpu,
      const BaseCva6CPUParams &params) : size(params.schedSize) {}
  bool canPush(Cva6DynInstPtr inst) override { return fifo.size() < size; }
  void push(Cva6DynInstPtr inst) override { fifo.push_back(inst); };
  bool canPop() override { return fifo.size(); };
  Cva6DynInstPtr front() override {return fifo.front(); };
  Cva6DynInstPtr pop() override {
    Cva6DynInstPtr ret = fifo.front();
    fifo.pop_front();
    return ret;
  };
  void flush() override { fifo.clear(); };
};

class MinLineAnalyserV2
{
  uint64_t themll = 0;
  uint64_t mll = 0; /* Min Last Line */
  uint64_t size; // For now the SQ size

  std::deque<uint64_t /* Times */> times;

  public:
  MinLineAnalyserV2(uint64_t size_) : size(size_) {}

  virtual bool isConstraints(Cva6DynInstPtr& inst){
    return !inst->isFault() && inst->staticInst->isStore();
  }

  virtual bool isTriggerPush(Cva6DynInstPtr& inst){
    return isConstraints(inst);
  }

  uint64_t getMinSchedulerTime(Cva6DynInstPtr& inst){
    // assert(isConstraints(inst));
    // if (times.size() == size){
    //   return times.front();
    // }
    return themll;
  }

  /* When instruction is scheduled */
  void onSchedule(Cva6DynInstPtr& inst, uint64_t time){
    /* By default update MLL MaxLastLine */
    mll = std::max(time, mll);
    /* push in fifo the schedule line if store */
    if (isTriggerPush(inst)){
      times.push_back(mll);
      /* Update mll if needed */
      if (times.size() > size){
        themll = times.front();
        times.pop_front();
      }
    }
  }

  void flush(){
    mll = 0; // Nothing in flight
    themll = 0;
    times.clear();
  }
};

struct scheduler_entry_t
{
  uint64_t pushed = 0;
  uint64_t load_pushed = 0;
  std::deque<Cva6DynInstPtr> slots;
  // PhysicalRegFile<unsigned int> holdregs;
  // PhysicalRegFile<unsigned int> latencyregs;

  public:
  inline static int width = 0;
  // std::map<uint64_t/* Addr */, uint64_t/* Count */> idealstoremap;
  bool canPush(){ return pushed < width; }
  void push(Cva6DynInstPtr inst, uint64_t latency=0){
    pushed++;
    load_pushed += !inst->isFault() && inst->staticInst->isLoad();
    slots.push_back(inst);
  }

  Cva6DynInstPtr front(){ return slots.front(); }
  size_t size(){ return slots.size(); }

  Cva6DynInstPtr pop(){
    Cva6DynInstPtr inst = slots.front();
    slots.pop_front();

    return inst;
  }
  bool contains(Cva6DynInstPtr inst){
    return find(slots.begin(), slots.end(), inst) != slots.end();
  }
  bool empty(){ return slots.empty(); }

  #if 0
  bool isRaWMem(uint64_t addr, Cva6DynInstPtr *store_inst) {
    bool ret = idealstoremap[addr | 0b111];
    if (ret){
      for (auto it = slots.rbegin(); it != slots.rend(); ++it){
        Cva6DynInstPtr inst = *it;
        uint64_t iaddr;
        if (isMemWrite(inst, iaddr) && ((iaddr | 0b111) == (addr | 0b111))){
          *store_inst = inst;
          return ret;
        }
      }
      fatal("Unrecheable: must hit inst");
    }
    return ret;
  }
  #endif
  // uint64_t execution_latency(PhysicalReg &reg){ return latencyregs[reg]; }
};

class SchedulerPierreMichaud : public BaseScheduler, public Named
{
  class MinLineAnalyser
  {
    uint64_t max_last_time = 0;
    uint64_t size; // For now the SQ size
    std::deque<uint64_t /* Times */> times;

    public:
    MinLineAnalyser(uint64_t size_) : size(size_) {}

    uint64_t getMinSchedulerTime(Cva6DynInstPtr& inst){
      return max_last_time;
    }

    /* When instruction is scheduled */
    void onSchedule(Cva6DynInstPtr& inst, uint64_t time){
      /* By default push in fifo the schedule line */
      times.push_back(time);
      /* Update max_last_time if needed */
      if (times.size() == size){
        max_last_time = std::max(times.front(), max_last_time);
        times.pop_front();
      }
    }

    void flush(){
      max_last_time = 0; // Nothing in flight
      times.clear();
    }
  };



  class MinLineAnalyserBB : public MinLineAnalyserV2
  {
    public:
    MinLineAnalyserBB(uint64_t size_) : MinLineAnalyserV2(size_) {}

    bool isConstraints(Cva6DynInstPtr& inst) override {
      return true;
    }

    bool isTriggerPush(Cva6DynInstPtr& inst) override {
      return !inst->isFault() && inst->staticInst->isControl();
    }
  };

  class MinLineDepAfterBB
  {
    PhysicalRegFile<uint8_t> fromload;
    uint64_t last_bb_time = 0;
    uint64_t cur_bb_time = 0;
    public:
    MinLineDepAfterBB() {}

    bool isConstraints(Cva6DynInstPtr& inst){
      for (auto &reg: inst->regs_src_phy){
        if (fromload[reg]){ return true; }
      }
      return false;
    }
    uint64_t getMinSchedulerTime(Cva6DynInstPtr& inst){
      return last_bb_time;
    }
    /* When instruction is scheduled */
    void onSchedule(Cva6DynInstPtr& inst, uint64_t time){
      cur_bb_time = std::max(cur_bb_time, time);
      bool is_load = !inst->isFault() && inst->staticInst->isLoad();
      is_load &= !inst->vp_data.value_ready;
      for (auto &reg: inst->regs_dst_phy){
        fromload[reg] = is_load;
      }
      /* When BB end, clear stats and setup last_bb_time */
      if (!inst->isFault() && inst->staticInst->isControl()){
        last_bb_time = cur_bb_time;
        fromload.setall(false);
      }
    }
  } mldabb;

  Cva6CPU &cpu;

  struct Stats : public statistics::Group
  {
    statistics::Scalar req;
    statistics::Scalar rescheduled;
    statistics::Scalar load_bypass_store;
    statistics::Scalar mdp_false_positive;
    statistics::Scalar mdp_true_positive;
    statistics::Scalar mdp_true_negative;
    statistics::Scalar mdp_false_negative;

    Stats(Cva6CPU &cpu) :
      statistics::Group(&cpu, "SchedulerPM"),
      ADD_STAT(req, ""),
      ADD_STAT(rescheduled, ""),
      ADD_STAT(load_bypass_store, "A load bypassed a previous store"),
      ADD_STAT(mdp_false_positive, ""),
      ADD_STAT(mdp_true_positive, ""),
      ADD_STAT(mdp_true_negative, ""),
      ADD_STAT(mdp_false_negative, "")
    { }
  } stats;


  /* The main containers */
  uint64_t size;
  size_t loadPerCycle;
  std::deque<scheduler_entry_t> s2d; // 2D array Scheduler
  uint64_t inflight_insts_count = 0;

  uint64_t last_serialisation_time = 0;
  uint64_t last_store_time = 0;
  uint64_t base_time = 0;

  PhysicalRegFile<unsigned int> timeofregready;
  // ArchRegFile<unsigned int> timeofregready;
  PhysicalRegFile<uint64_t> maxtimeoflasttouch;

  MinLineAnalyserV2 mla;
  MinLineAnalyserBB mlabb;

  PhysicalRegFile<uint64_t> physical2arch;

  /* FU ready constraint */
  public:
  class FUModel
  {
    // We have 2 divisor
    const size_t count = 2;
    uint64_t next_ready_time[2] = { 0 };
    // Instructions that use the fu
    std::vector<OpClass> set = { OpClass::FloatDiv,
                                 OpClass::FloatSqrt };

    bool isTrigger(Cva6DynInstPtr& inst){
      if (inst->isFault()){
        return 0;
      }
      for (OpClass &cl: set){
        if (inst->staticInst->opClass() == cl){
          return true;
        }
      }
      return false;
    }
    public:
    void onSchedule(Cva6DynInstPtr& inst, uint64_t time){
      if (isTrigger(inst)){
        *minReadyTimePtr() = time + instructioncoststatic(inst);
      }
    }

    uint64_t* minReadyTimePtr(){
      uint64_t *min = next_ready_time;
      for (size_t i = 1; i < count; i++){
        if (next_ready_time[i] < *min){
          min = &next_ready_time[i];
        }
      }
      return min;
    }

    uint64_t getRT(Cva6DynInstPtr &inst) {
      return isTrigger(inst) ? *minReadyTimePtr() : 0;
    }
  } fumodel;

  // DEBUG: delme !
  uint64_t find_inst_line(Cva6DynInstPtr inst);
  /* Base primitives */
  uint64_t getSourceUseLine(PhysicalReg &reg);

  uint64_t getSLFU(Cva6DynInstPtr &inst); /* FU deps */
  uint64_t getSLRR(Cva6DynInstPtr &inst); /* Dataflow deps */
  uint64_t getSLMDP(Cva6DynInstPtr &inst); /* MDP deps */
  uint64_t getSLSTORE(Cva6DynInstPtr &inst); /* Store Set */

  uint64_t getScheduleLine(Cva6DynInstPtr inst, uint64_t &delta);
  // uint64_t getScheduleLineForLoadAddr(uint64_t addr, Cva6DynInstPtr*inst);
  uint64_t latency(Cva6DynInstPtr inst){
    return instructioncoststatic(inst);
  }

  public:
  /* Interface */
  bool canPush(Cva6DynInstPtr inst) override {
    return inflight_insts_count < size &&
           s2d.size() < size; // Avoid huge array
  }

  void push(Cva6DynInstPtr inst) override;
  Cva6DynInstPtr front() override {
    assert(inflight_insts_count);
    for (auto e: s2d){
      if (!e.empty()){
        return e.front();
      }
    }
    fatal("Unrecheable!\n");
    // assert(s2d.size());
    // assert(s2d.front().size());
    // return s2d.front().front();
  };
  protected:
  Cva6DynInstPtr pop_front(){
    assert(inflight_insts_count);
    for (auto& e: s2d){
      if (!e.empty()){
        return e.pop();
      }
    }
    fatal("Unrecheable!\n");
  }

  public:
  Cva6DynInstPtr pop() override;
  bool canPop() override { return inflight_insts_count; }
  // bool canPop() override { return s2d.size() && !s2d.front().empty(); }
  void flush() override {
    /* Set base time to final time (clean debug)*/
    base_time += s2d.size();
    /* Clear everything */
    s2d.clear();
    inflight_insts_count = 0;
    last_store_time = 0;
    last_serialisation_time = 0;
    timeofregready.setall(0);
  }

  bool canRenameDest(Cva6DynInstPtr &inst,
    std::deque<uint64_t> &FL, uint64_t &preg) override;

  void tick() override {
    /* Try to increase base time */
    while (!s2d.empty() && s2d.front().empty()){
      s2d.pop_front();
      base_time ++;
      DPRINTF(Cva6Sched, "SCHED L0 <-> T%ld\n", base_time);
    }
  }

  /* Constructor */
  SchedulerPierreMichaud(
    const std::string &name,
    Cva6CPU &cpu_,
    const BaseCva6CPUParams &p
  ) : Named(name), cpu(cpu_),
      stats(cpu_), size(p.schedSize), loadPerCycle(p.loadPerCycle),
      mla(16), mlabb(6) {
        scheduler_entry_t::width = p.schedWidth;
      }
};


} // namespace cva6
} // namespace gem5

