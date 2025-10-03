/**
 * @file schedulerbb.hh
 * @author Pierre Ravenel (pravenel@kalrayinc.com)
 * @brief BasicBlock FIFO scheduler
 * @version 0.1
 * @date 2025-07-08
 *
 */


#pragma once

#include "cpu/cva6/pipeline.hh"
#include "cpu/cva6/scheduler.hh"

namespace gem5 {
namespace cva6 {

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
  bool canPush(Cva6DynInstPtr inst) override { return true; /* TODO */ }
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


} // namespace cva6
} // namespace gem5
