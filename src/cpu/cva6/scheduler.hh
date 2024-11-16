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
#include "debug/Cva6Sched.hh"
#include "debug/Cva6SchedSched.hh"

namespace gem5 {
namespace cva6 {

#define NB_INFLIGHTS 100
#define SCHED_IGNORE_BRANCH 1
#define NBBBQ 4

std::string dumpInstPreg(Cva6DynInstPtr& inst);
uint64_t instructioncoststatic(Cva6DynInstPtr inst);

inline bool isBBend(Cva6DynInstPtr& inst){
  return !inst->isFault() && inst->staticInst->isControl();
}

inline bool isMemWrite(Cva6DynInstPtr& inst, uint64_t &paddr){
  if (!inst->isFault()
     && inst->staticInst->isMemRef()
     && !inst->staticInst->isLoad()){
      fatal_if(!inst->dreq, "Must have dreq: %s\n", *inst);
      paddr = inst->dreq->getPaddr();
    return true;
  }
  return false;
}

inline bool isMemLoad(Cva6DynInstPtr& inst, uint64_t &paddr){
  if (!inst->isFault()
     && inst->staticInst->isLoad()){
      assert(inst->dreq);
      paddr = inst->dreq->getPaddr();
    return true;
  }
  return false;
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

  std::vector<T> array;
  // std::unordered_map<PhysicalReg, T, PhysicalRegHash_t> map;
  public:
  PhysicalRegFile() {}

  T& operator[](PhysicalReg reg){
    fatal_if(!reg.isRenammed, "Reg is not Physical : %s\n", reg);
    fatal_if(!reg.classValue, "Reg is not valid : %s\n", reg);
    // Fix size;
    if (array.size() <= reg.phys_reg_idx){
      array.resize(reg.phys_reg_idx + 1);
    }
    return array[reg.phys_reg_idx];
  }
};

template <class T>
class VirtualRegFile
{
  class Hash
  {
  public:
    size_t operator()(const PhysicalReg &p) const {
      return p.virt_reg_idx;
    }
  };

  class KeyEqual
  {
  public:
    size_t operator()(const PhysicalReg &p1, const PhysicalReg &p2) const {
      return p1.virt_reg_idx == p2.virt_reg_idx;
    }
  };

  std::unordered_map<PhysicalReg, T, Hash, KeyEqual> map;
  public:
  VirtualRegFile() {}
  T& operator[](PhysicalReg reg){
    assert(reg.classValue);
    return map[reg];
  }
};


class PhysicalRegAllocator
{
  std::deque<uint64_t> free_list;
  uint64_t cur_reg = 0;
  VirtualRegFile<uint64_t> rmt;

  public:
  PhysicalRegAllocator(uint64_t nb_regs) {
    for (int i = 0; i < nb_regs; i++){
      free_list.push_back(i);
    }
  }

  void rename(Cva6DynInstPtr& inst){
    /* Rename srcs */
    for (PhysicalReg& reg: inst->regs_src_phy){
      reg.doRename(rmt[reg]);
    }
    /* Allocate destinations regs */
    for (PhysicalReg& reg: inst->regs_dst_phy){
      uint64_t pregidx = (cur_reg % 2048);
      rmt[reg] = pregidx;
      reg.doRename(pregidx);
      cur_reg++;
    }
  }

  // bool can_alloc(){
  //   return free_list.size();
  // }

  // uint64_t reg_alloc() {
  //   assert(free_list.size());
  //   uint64_t ret = free_list.front();
  //   free_list.pop_front();
  //   return ret;
  // }

  // void reg_free(uint64_t pidx){
  //   free_list.push_back(pidx);
  // }

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
    DPRINTF(Cva6Sched, "CLOCK(%d) += STALL(%d) :: -> +READY(%d):%d : %s\n",
      clock, delta_time, delta_finish, readytime, *inst);
    return delta_time;
  }
};

class BaseScheduler
{
  public:
  virtual void push(Cva6DynInstPtr inst) = 0;
  virtual Cva6DynInstPtr pop() = 0;
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
  void push(Cva6DynInstPtr inst) override;
  Cva6DynInstPtr pop() override;
  private:
  Cva6DynInstPtr internal_pop();
};


class SchedulerPierreMichaud : public BaseScheduler, public Named
{
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

  struct scheduler_entry_t
  {
    std::deque<Cva6DynInstPtr> slots;
    PhysicalRegFile<unsigned int> holdregs;
    PhysicalRegFile<unsigned int> latencyregs;
    std::map<uint64_t/* Addr */, uint64_t/* Count */> idealstoremap;
    void push(Cva6DynInstPtr inst, uint64_t latency){
      slots.push_back(inst);
      /* Set Regs annotation */
      for (auto &reg: inst->regs_dst_phy){
        holdregs[reg] += 1;
        latencyregs[reg] = latency;
      }
      /* Set Memory annotations */
      uint64_t addr;
      if (isMemWrite(inst, addr)){
        idealstoremap[addr | 0b111] += 1;
      }
    }
    Cva6DynInstPtr pop(){
      Cva6DynInstPtr inst = slots.front();
      slots.pop_front();
      /* Unset Regs annotation */
      for (auto &reg: inst->regs_dst_phy){
        holdregs[reg] -= 1;
      }
      /* Unset Memory annotations */
      uint64_t addr;
      if (isMemWrite(inst, addr)){
        idealstoremap[addr | 0b111] -= 1;
      }
      return inst;
    }
    bool contains(Cva6DynInstPtr inst){
      return find(slots.begin(), slots.end(), inst) != slots.end();
    }
    bool empty(){ return slots.empty(); }
    bool isRaW(PhysicalReg &reg){ return holdregs[reg]; }
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
    uint64_t execution_latency(PhysicalReg &reg){ return latencyregs[reg]; }

  };

  /* The main containers */
  std::deque<scheduler_entry_t> s2d; // 2D array Scheduler

  StoreSet<Cva6DynInstPtr> mdp;
  // DEBUG: delme !
  uint64_t find_inst_line(Cva6DynInstPtr inst);
  /* Base primitives */
  uint64_t getSourceUseLine(PhysicalReg &reg);
  uint64_t getScheduleLine(Cva6DynInstPtr inst);
  uint64_t getScheduleLineForLoadAddr(uint64_t addr, Cva6DynInstPtr*inst);
  uint64_t latency(Cva6DynInstPtr inst){
    return instructioncoststatic(inst);
  }

  public:
  /* Interface */
  void push(Cva6DynInstPtr inst) override;
  Cva6DynInstPtr pop() override;
  /* Constructor */
  SchedulerPierreMichaud(
    const std::string &name,
    Cva6CPU &cpu,
    const BaseCva6CPUParams &params
  ) : Named(name), stats(cpu), mdp(1024) {}
};


} // namespace cva6
} // namespace gem5

