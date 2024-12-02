/**
 * @file scoreboard.hh
 * @author Pierre Ravenel (pravenel@kalrayinc.com)
 * @brief
 * @version 0.1
 * @date 2023-05-25
 *
 * A simple instruction scoreboard for tracking dependencies in Execute.
 *
 * (i) First step is to insert instruction in scoreboard.
 * For now instruction have 3 state : (None, Issued, Completed)
 *
 * canPush()       ----> push()
 *
 * (ii) Second step is to issue instructions if possible.
 * We have to check if register sources and destinations are available/
 *
 * getIssueInst()  ----> issueInst()
 *
 * (iii) When "FU completed" instruction we must notify the scoreboard
 *
 * "FU completed"  -----> completeInst()
 *
 * (iv) When instructions are completed we can commit them
 *
 * getCommitInst() ----> commitInst()
 *
 *
 */

#pragma once

#include <vector>

#include "base/named.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/reg_class.hh"

namespace gem5 {
namespace cva6 {

class Scoreboard : public Named
{
  public:
    Cva6CPU &cpu;

    /* number of entries un issue queue*/
    const unsigned nr_entries;

protected:
    /* this is the FIFO struct of the issue queue  */
    std::deque<Cva6DynInstPtr> issue_queue;

    enum reg_state_t
    {
      FREE,
      IN_USE,
      FWABLE
    };
    PhysicalRegFile<reg_state_t> sb;
    PhysicalRegFile<uint64_t> prf;

    // RAW valid <=> state in {FREE, FWABLE}
    // WAW valid <=> state in {FREE}

  public:
    Scoreboard(const std::string &name,
               Cva6CPU &cpu_, uint64_t size) :
        Named(name),
        cpu(cpu_),
        nr_entries(size) { }

  protected:

  public:
    bool isUnissedStoreBefore(Cva6DynInstPtr inst_in);
    /** Returns the register state with associated value */
    bool getRegState(Cva6DynInstPtr inst_in, PhysicalReg& reg);
  protected:
    /** Can this instruction be issued.  Are any of its source registers
     *  due to be written by other marked-up instructions in flight */
    bool canInstIssue(Cva6DynInstPtr inst);

  public:
    /** Is Available space in scoreboard */
    bool canPush();
    /** push inst in the scoreboard */
    void pushInst(Cva6DynInstPtr inst);

    /** Issue Stage */
    /* Return the instruction to issue. Bubble if none. */
    Cva6DynInstPtr getIssueInst(size_t index, bool &is_oser, bool &is_ready);
    /** Issue the instruction (notify instruction is issued) */
    void issueInst(Cva6DynInstPtr inst);

    /** Notify scoreboard functional unit finished */
    void completeInst(Cva6DynInstPtr inst);

    /** Return the instruction to commit. Bubble is none. */
    Cva6DynInstPtr getCommitInst(size_t index=0);
    /** Commit the instruction */
    void commitInst(Cva6DynInstPtr inst);

    /** Tick the scoreboard: evaluate flip flops*/
    void tick();
    /** Flush issue queue and clear down all the reg dependencies */
    void flush();

    /* Misspredict inst_error: reset scoreboard.
     * Backend must flush from returned instruction */
    // Cva6DynInstPtr flush_value_from(Cva6DynInstPtr inst_error,
    //    bool force=false);

    bool is_fence_issued(){
      for (Cva6DynInstPtr inst: issue_queue){
        if (!inst->issue_completed){
          return false;
        }
        if (!inst->isFault() &&
          (inst->staticInst->isReadBarrier() ||
          inst->staticInst->isWriteBarrier())){
            return true;
        }
      }
      return false;
    }

    std::deque<Cva6DynInstPtr> &getIssueQueue(){
      return issue_queue;
    }

    bool markMemoryViolation(Cva6DynInstPtr inst);

    bool isInstInFu(Cva6DynInstPtr inst){
      return !inst->execute_completed && inst->issue_completed;
    }

    void dump();


};

} // namespace cva6
} // namespace gem5
