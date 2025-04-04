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

class PhysicalRegAllocator
{
  size_t n;
  std::deque<uint64_t> free_list;
  std::deque<uint64_t> free_list_popped;

  ArchRegFile<uint64_t> rmt;
  ArchRegFile<Cva6DynInstPtr> rmt_owner;
  ArchRegFile<uint64_t> rmt_checkpoint;

  bool incarchreg; // Scoreboard arch or Full RR
  bool freeregdead;
  public:
  PhysicalRegAllocator(uint64_t nb_regs, bool incarchreg_, bool freeregdead_) :
    n(nb_regs), incarchreg(incarchreg_), freeregdead(freeregdead_) {
    fatal_if(!incarchreg && nb_regs >= NB_I2ID, "Need more preg");
    // Default RMT
    for (int i = 0; i < 64; i++){
      RegId regid = i2id(i);
      PhysicalReg reg(regid);
      rmt[reg] = 1000;
    }
    /* Initialise all arch regs to physical mapping */
    for (int i = 0; i < nb_regs; i++){
      if (incarchreg && i < NB_I2ID){
        RegId regid = i2id(i);
        PhysicalReg reg(regid);
        rmt[reg] = i;
      } else { /* Let the register for future use */
        free_list.push_back(i);
      }
    }
    rmt_checkpoint = rmt; // Default rmt !
  }

  bool canRename(){
    if (!free_list.size()){
      DPRINTF(Cva6Rename, "Out of PREG\n");
    }
    return free_list.size();
  }

  void rename(Cva6DynInstPtr& inst){
    /* Rename srcs */
    for (PhysicalReg& reg: inst->regs_src_phy){
      reg.doRename(rmt[reg]);
    }

    /* Allocate destinations regs and rename */
    for (PhysicalReg& reg: inst->regs_dst_phy){
      /* Keep track of old mapping for reg free */
      PhysicalReg freereg = reg; // Ok because rmt [ ArchReg ]
      freereg.doRename(rmt[reg]);
      inst->phys_reg_to_free.push_back(freereg);
      /* pop register from free list */
      fatal_if(!free_list.size(), "No more entry in FL\n");
      uint64_t pregidx = free_list.front();
      free_list.pop_front();
      /* Maintain speculative state */
      free_list_popped.push_back(pregidx);
      /* Finally rename the register */
      reg.doRename(pregidx);
      DPRINTF(Cva6Rename, "Alloc reg %s\n", reg);

      /* Setup the speculative mapping in RMT */
      /* Secure check */
      if (incarchreg){
        // std::cout << "renamed " << reg << std::endl;
        for (int i = 0; i < NB_I2ID; i++){
          RegId regid = i2id(i);
          PhysicalReg r2(regid);
          fatal_if(rmt[r2] == pregidx, "(try rename %s to %d) "
            "Reg %s already mapped to %s\n", reg, pregidx, r2, pregidx);
        }
      }
      rmt[reg] = pregidx;
      rmt_owner[reg] = inst;
    }
  }
  private:

  void free_reg(PhysicalReg& reg){
    // std::cout << "Free " << idx << std::endl;
    DPRINTF(Cva6Rename, "Free reg %s\n", reg);
    if (reg.phys_reg_idx != 1000){
      free_list.push_back(reg.phys_reg_idx);
      /* Invalidate RMT */
      // TODO : is this mandatory (Could we wait until next realloc)?

      // rmt[reg] = 1000;
      // rmt_owner[reg] = Cva6DynInst::bubble();
    }
  }

  void free(Cva6DynInstPtr& inst){
    /* Free registers WaW */
    for (PhysicalReg& reg: inst->phys_reg_to_free){
      free_reg(reg);
    }
    /* Can we free RDeadaW ? */
    if (freeregdead){
      for (PhysicalReg& reg: inst->regs_src_phy){
        if (reg.is_reg_dead){ // TODO
          free_reg(reg);
        }
      }
    }
  }
  public:
  void speculative_update(Cva6DynInstPtr& inst){
    if (!incarchreg){ // If no arch reg
      free(inst);
    }
  }
  /* Free all registers */
  void commit(Cva6DynInstPtr& inst){
    if (!incarchreg){ // If no arch reg
      /* free_rd_at_commit */
      for (PhysicalReg& reg: inst->regs_dst_phy){
        if (rmt_owner[reg] == inst){ // We are the owner
          free_reg(reg);
          rmt[reg] = 1000;
          /* !!! Avoid duplicate in RMT !!! */
          /* If there is duplicate
           * -> Multiple FREE of the same preg
           * -> Duplicate in FL
           * -> 2 arch reg may have the same preg !
           */
          // Instead of this we may defer the RMT invalidation.
          // This can be done at allocate. Do do this, it requires
          // to know the old arch reg.
        }
      }
      return;
    }
    free(inst);
    /* Update checkpoint with new allocations */
    for (PhysicalReg& reg: inst->regs_dst_phy){
      /* Assume InO commit */
      // std::cout << "ALLOC " << reg << " old was:
      //  " << rmt_checkpoint[reg] << std::endl;
      fatal_if(free_list_popped.front() != reg.phys_reg_idx, "Bad Free\n");
      free_list_popped.pop_front();
      rmt_checkpoint[reg] = reg.phys_reg_idx;
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
      rmt.setall(1000);
      // And reset FL
      free_list_popped.clear();
      free_list.clear();
      for (int i = 0; i < n; i++){
        free_list.push_back(i);
      }
    }

  }
};


struct waitqueue_t
{
  std::deque<Cva6DynInstPtr> holdqueue;
  PhysicalRegFile<unsigned int> holdregs;

  waitqueue_t() {}

  void push(Cva6DynInstPtr inst){

    /* Try to rename to flying registers */
    /* Must be done before inserting instruction in the queue */
    // tryRenameInst(inst);

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
    #if 0
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
    #endif
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
          // regalloc.reg_free(reg.phys_reg_idx);
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
    uint64_t clock_before = clock;
    if (delta_time == 0){
      subclock += 1;
      const int issueWidth = 4;
      if (subclock == issueWidth){
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
    // DPRINTF(Cva6Sched, "CLOCK(%d) += STALL(%d) :: -> +READY(%d):%d : %s\n",
    //   clock, delta_time, delta_finish, readytime, *inst);
    return delta_time;
  }
};

class BaseScheduler
{
  public:
  virtual bool canPush() = 0;
  virtual void push(Cva6DynInstPtr inst) = 0;
  virtual bool canPop() = 0;
  virtual Cva6DynInstPtr front() = 0;
  virtual Cva6DynInstPtr pop() = 0;
  virtual void flush() = 0;
  virtual void violation(uint64_t store_pc, uint64_t load_pc){
    /* Default: do nothing */
  }
};

class NoScheduler : public BaseScheduler
{
  uint64_t size;
  std::deque<Cva6DynInstPtr> fifo;
  public:
  NoScheduler(const std::string &name,
      Cva6CPU &cpu,
      const BaseCva6CPUParams &params) : size(params.schedSize) {}
  bool canPush() override { return fifo.size() < size; }
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

/**
 * My custom Basic Block scheduler
 */
class SchedulerBB : public BaseScheduler
{
  struct Stats : public statistics::Group
  {
    statistics::Scalar req;
    statistics::Scalar rescheduled;

    Stats(Cva6CPU &cpu) :
      statistics::Group(&cpu, "SchedulerBB"),
      ADD_STAT(req, ""),
      ADD_STAT(rescheduled, "")
    { }
  } stats;

  /* Scheduler onternals */
  std::deque<waitqueue_t*> bbq;
  std::deque<Cva6DynInstPtr> tempoq;
  bool wait_drain = false;
  StreamAnalyser mainsa;
  public:
  SchedulerBB(const std::string &name,
      Cva6CPU &cpu,
      const BaseCva6CPUParams &params) :
    stats(cpu),
    mainsa(cpu, "stream.sa", SCHED_IGNORE_BRANCH){
    for (int i = 0; i < NBBBQ; i++){
        bbq.push_back(new waitqueue_t());
      }
    }
  /* Interface */
  bool canPush() override { return true; /* TODO */ }
  void push(Cva6DynInstPtr inst) override;
  Cva6DynInstPtr pop() override;
  // TODO
  Cva6DynInstPtr front() override { return Cva6DynInst::bubble(); };
  bool canPop() override { return true; /* TODO */}
  private:
  Cva6DynInstPtr internal_pop();
};

// template<class T>
// class RibbonBuffer {
//   uint64_t base_index = 0;
//   std::deque<T> ribbon;

//   inline uint64_t norm_index(uint64_t idx){
//     assert(idx >= base_index);
//     return idx - base_index;
//   }
//   T& operator[](uint64_t idx){
//     return ribbon[norm_index(idx)];
//   }

//   void push(T& e){

//   }
// };
class scheduler_entry_t
{
  uint64_t pushed = 0;
  std::deque<Cva6DynInstPtr> slots;
  // PhysicalRegFile<unsigned int> holdregs;
  // PhysicalRegFile<unsigned int> latencyregs;

  public:
  inline static int width = 0;
  // std::map<uint64_t/* Addr */, uint64_t/* Count */> idealstoremap;
  bool canPush(){ return pushed < width; }
  void push(Cva6DynInstPtr inst, uint64_t latency=0){
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
  std::deque<scheduler_entry_t> s2d; // 2D array Scheduler
  uint64_t inflight_insts_count = 0;

  uint64_t last_serialisation_time = 0;
  uint64_t last_store_time = 0;
  uint64_t base_time = 0;

  StoreSet<Cva6DynInstPtr> mdp;

  PhysicalRegFile<unsigned int> timeofregready;
  // ArchRegFile<unsigned int> timeofregready;
  PhysicalRegFile<uint64_t> maxtimeoflasttouch;

  MinLineAnalyserV2 mla;
  MinLineAnalyserBB mlabb;

  // DEBUG: delme !
  uint64_t find_inst_line(Cva6DynInstPtr inst);
  /* Base primitives */
  uint64_t getSourceUseLine(PhysicalReg &reg);

  uint64_t getSLRR(Cva6DynInstPtr &inst);
  uint64_t getSLMDP(Cva6DynInstPtr &inst);
  uint64_t getSLSTORE(Cva6DynInstPtr &inst);
  uint64_t getSLBBdep(Cva6DynInstPtr &inst);

  uint64_t getScheduleLine(Cva6DynInstPtr inst, uint64_t &delta);
  // uint64_t getScheduleLineForLoadAddr(uint64_t addr, Cva6DynInstPtr*inst);
  uint64_t latency(Cva6DynInstPtr inst){
    return instructioncoststatic(inst);
  }

  public:
  /* Interface */
  bool canPush() override {
    return inflight_insts_count < size &&
           s2d.size() < size; // Avoid huge array
  }
  void push(Cva6DynInstPtr inst) override;
  Cva6DynInstPtr front() {
    assert(inflight_insts_count);
    assert(s2d.size());
    assert(s2d.front().size());
    return s2d.front().front();
  };
  Cva6DynInstPtr pop() override;
  bool canPop() override { return inflight_insts_count; }
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
  void violation(uint64_t store_pc, uint64_t load_pc) override {
    mdp.violation(store_pc, load_pc);
  }
  /* Constructor */
  SchedulerPierreMichaud(
    const std::string &name,
    Cva6CPU &cpu_,
    const BaseCva6CPUParams &p
  ) : Named(name), cpu(cpu_),
      stats(cpu_), size(p.schedSize), mdp(1024),
      mla(16), mlabb(6) {
        scheduler_entry_t::width = p.schedWidth;
      }
};


} // namespace cva6
} // namespace gem5

