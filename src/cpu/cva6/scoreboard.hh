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
    const BaseISA::RegClasses regClasses;

    const unsigned intRegOffset;
    const unsigned floatRegOffset;
    const unsigned ccRegOffset;
    const unsigned vecRegOffset;
    const unsigned vecPredRegOffset;

    /** The number of registers in the Scoreboard.  These
     *  are just the integer, CC and float registers packed
     *  together with integer regs in the range [0,NumIntRegs-1],
     *  CC regs in the range [NumIntRegs, NumIntRegs+NumCCRegs-1]
     *  and float regs in the range
     *  [NumIntRegs+NumCCRegs, NumFloatRegs+NumIntRegs+NumCCRegs-1] */
    const unsigned numRegs;

    /** Type to use when indexing numResults */
    typedef unsigned short int Index;

    /* number of entries un issue queue*/
    const unsigned nr_entries;

    /* destination reg usage in issue_queue */
    enum DestRegState
    {
      FREE,     // Nothing
      IN_USE,   // In EX state
      FWABLE,   // Finished EX stage but not comitted
      COMMIT,   // Finished EX stage and comitted
    };

protected:
    /* this is the FIFO struct of the issue queue  */
    std::deque<Cva6DynInstPtr> issue_queue;

    // RAW valid <=> state in {FREE, FWABLE}
    // WAW valid <=> state in {FREE}

  public:
    Scoreboard(const std::string &name,
               Cva6CPU &cpu_, uint64_t size) :
        Named(name),
        cpu(cpu_),
        regClasses(cpu.thread->getIsaPtr()->regClasses()),
        intRegOffset(0),
        floatRegOffset(intRegOffset + regClasses.at(IntRegClass)->numRegs()),
        ccRegOffset(floatRegOffset + regClasses.at(FloatRegClass)->numRegs()),
        vecRegOffset(ccRegOffset + regClasses.at(CCRegClass)->numRegs()),
        vecPredRegOffset(vecRegOffset +
                regClasses.at(VecElemClass)->numRegs()),
        numRegs(vecPredRegOffset + regClasses.at(VecPredRegClass)->numRegs()),
        nr_entries(size),
        issue_queue()
   { }

  protected:
    /** Flatten a RegId, irrespective of what reg type it's pointing to */
    RegId flattenRegIndex(const RegId& reg);

    /** Sets scoreboard_index to the index into numResults of the
     *  given register index.  Returns true if the given register
     *  is in the scoreboard and false if it isn't */
    bool findIndex(const RegId& reg, Index &scoreboard_index);


    DestRegState getRegStateOld(RegId reg, RegVal &val);

    /** Returns the register state with associated value */
    DestRegState getRegState(Cva6DynInstPtr inst_in, RegId reg, RegVal &val);

    /** Forward a register. If register is in the scoreboard it must
     * be forwardable. */
    bool forward(Cva6DynInstPtr inst_in, RegId reg, RegVal &val);

    /** Can this instruction be issued.  Are any of its source registers
     *  due to be written by other marked-up instructions in flight */
    bool canInstIssue(Cva6DynInstPtr inst);

  public:

    /** Is Available space in scoreboard */
    bool canPush();
    /** push inst in the scoreboard */
    void pushInst(Cva6DynInstPtr inst);

    /* Return the instruction to issue. Bubble if none. */
    Cva6DynInstPtr getIssueInst(size_t index, bool &is_over_serialise,
      bool &is_ready);

    /** Issue the instruction */
    void issueInst(Cva6DynInstPtr inst, SimpleThread &thread);

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
    Cva6DynInstPtr flush_value_from(Cva6DynInstPtr inst_error,
       bool force=false);

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

    bool isInstInFu(Cva6DynInstPtr inst){
      return !inst->execute_completed && inst->issue_completed;
    }

    Cva6DynInstPtr getHeadInst(){
      if (issue_queue.empty()){
        return Cva6DynInst::bubble();
      }
      return issue_queue.front();
    }

    void dump();


};

} // namespace cva6
} // namespace gem5
