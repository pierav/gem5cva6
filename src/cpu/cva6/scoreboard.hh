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
    struct Stats : public statistics::Group
    {
      statistics::Scalar reg_read;
      statistics::Scalar reg_read_fw;
      statistics::Scalar reg_read_commit;
      statistics::Scalar reg_read_commit_unsafe;
      statistics::Scalar reg_write_fw;
      Stats(const std::string &name, BaseCPU &cpu) :
        statistics::Group(&cpu, name.c_str()),
        ADD_STAT(reg_read, ""),
        ADD_STAT(reg_read_fw, ""),
        ADD_STAT(reg_read_commit, ""),
        ADD_STAT(reg_read_commit_unsafe, ""),
        ADD_STAT(reg_write_fw, "") {}
    } stats;

    /* number of entries un issue queue*/
    const unsigned nr_entries;

  protected:
    /* this is the FIFO struct of the issue queue  */
    std::deque<Cva6DynInstPtr> issue_queue;

    // FSM reg_state_t
    //                          Commit            ┌───────────┐
    //        │                  ┌─────┐          │   ERROR   │
    //  ┌─────▼────┐          ┌──┴─────▼──┐       └─────▲─────┘
    //  │   FREE   ├──────────►   InUSE   ├─────────────┘
    //  └──────────┘ Issue    └──┬─────▲──┘    Issue
    //                           │     │
    //               ExComplete  │     │ Issue
    //                           │     │
    //                        ┌──▼─────┴──┐
    //                        │    FW     │
    //                        └──┬─────▲──┘
    //                           └─────┘
    //                           Commit

    enum reg_state_t
    {
      FREE,
      IN_USE,
      FWABLE
    };
    PhysicalRegFile<reg_state_t> sb;
    PhysicalRegFile<Cva6DynInstPtr> _sb_producer; /* (debug) */
    ArchRegFile<uint8_t> _sb_is_unsafe;

    PhysicalRegFile<uint64_t> prf;
    PhysicalRegFile<uint8_t> prf_isfault;
    PhysicalRegFile<uint8_t> prf_isvp; /* Is value predicted (debug) */

    int is_serialise_inflight = 0;

    // RAW valid <=> state in {FREE, FWABLE}
    // WAW valid <=> state in {FREE}
  public:
    void forwardSpeculativeRegVal(PhysicalReg &reg, RegVal regval){
      /* Register must be free (Inst is not issued) */
      assert(sb[reg] == FREE);
      /* Mark forwardable */
      sb[reg] = FWABLE;
      prf_isvp[reg] = true;
      /* Forward value */
      prf[reg] = regval;
    }


  public:
    Scoreboard(const std::string &name, Cva6CPU &cpu_, uint64_t size) :
        Named(name),
        cpu(cpu_),
        stats(name, cpu_),
        nr_entries(size) { }

  protected:

  public:
    bool isUnissedStoreBefore(Cva6DynInstPtr inst_in);
    /** Returns the register state with associated value */
    bool getRegState(Cva6DynInstPtr inst_in, PhysicalReg& reg,
      Cva6DynInstPtr &producer);
  protected:

  public:
    /** Is Available space in scoreboard */
    bool canPush();
    /** push inst in the scoreboard */
    void pushInst(Cva6DynInstPtr inst);

    /** Issue Stage */
    /* Return the instruction to issue. Bubble if none. */
    /** Can this instruction be issued.  Are any of its source registers
     *  due to be written by other marked-up instructions in flight */
    Cva6DynInstPtr getIssueInst(
      bool &is_over_serialise,
      bool &is_raw,
      Cva6DynInstPtr &producer,
      bool &is_waw
    );

    /** Issue the instruction (notify instruction is issued) */
    void issueInst(Cva6DynInstPtr inst);

    /** Notify scoreboard functional unit finished */
    void completeInst(Cva6DynInstPtr inst);

    /** Return the instruction to commit. Bubble is none. */
    Cva6DynInstPtr getCommitInst(size_t index=0);
    /** Commit the instruction */
    void pre_commit(Cva6DynInstPtr inst){
      for (auto& reg: inst->regs_dst_phy){
        _sb_is_unsafe[reg] = true;
      }
    }
    void commitInst(Cva6DynInstPtr inst){
      assert(!inst->commit_completed); // Already commited
      inst->commit_completed = true;

      for (auto& reg: inst->regs_dst_phy){
        _sb_is_unsafe[reg] = false;
      }
      /* There is no need to free the register !! */
      /* Free registers  */
      // for (PhysicalReg &reg: inst->regs_dst_phy){
      //     assert(sb[reg] == FWABLE);
      //     sb[reg] = FREE;
      // }
    }

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
