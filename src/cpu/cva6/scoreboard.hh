/**
 * @file scoreboard.hh
 * @author Pierre Ravenel (pravenel@kalrayinc.com)
 * @brief
 * @version 0.1
 * @date 2023-05-25
 *
 * A simple instruction scoreboard for tracking dependencies
 *
 * *-*-*-*-*-*-*-*-*-*-* PRE ISSUE  *-*-*-*-*-*-*-*-*-*-*
 * (0)
 * onInsert                : Mark register reservation
 *
 * *-*-*-*-*-*-*-*-*-*-*  ISSUE  *-*-*-*-*-*-*-*-*-*-*
 * (i) First step is to issue instructions if possible.
 * We have to check if register sources and destinations are available
 *
 * getIssueInst()  ----> issueInst()
 *
 *   getIssueInst(...)     : Get the instruction to be issued
 *    |- chechIssueInst    : is the instruction ready to be issued
 *    |   |- getRegState   : is the register ready
 *  issueInst()            : Is the instruction is ready, issue it
 *    |- completeIssueInst : Update internals scorebords
 *
 *  *-*-*-*-*-*-*-*-*-*-* EXECUTE *-*-*-*-*-*-*-*-*-*-*
 * (ii) When "FU completed" instruction we must notify the scoreboard
 *
 * "FU completed"  -----> completeInst()
 *
 *  isInstInFu             : Is inst still in FU
 *  completeInst           : Write Back inst
 *
 *  *-*-*-*-*-*-*-*-*-*-* COMMIT *-*-*-*-*-*-*-*-*-*-*
 * (iii) When instructions are completed we can commit them
 *
 * getCommitInst() ----> commitInst()
 *
 *  pre_commit             :
 *  commit                 : (debug)
 *
 */

#pragma once

#include <vector>
#include "base/named.hh"
#include "base/statistics.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/pipe_data.hh"
#include "debug/Cva6Scoreboard.hh"

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

  ForwardInstDataPopIntf& inp;

  public:
    Scoreboard(const std::string &name, Cva6CPU &cpu_, uint64_t size,
        ForwardInstDataPopIntf& inp_) :
        Named(name),
        cpu(cpu_),
        stats(name, cpu_),
        nr_entries(size),
        inp(inp_) { }

  protected:

  public:

  protected:

  protected:
    /* insert inst in the scoreboard. Must be called only one time*/
    void onInsert(Cva6DynInstPtr inst);
    /* issue helper functions */
    /** Returns the register state with associated value */
    bool getRegState(Cva6DynInstPtr inst_in, PhysicalReg& reg,
      Cva6DynInstPtr &producer);
    bool chechIssueInst(Cva6DynInstPtr inst, bool &is_raw,
      Cva6DynInstPtr &producer, bool &is_waw);
    void completeIssueInst(Cva6DynInstPtr inst);

    void writeBackRF(Cva6DynInstPtr& inst);

  public:
    /** Issue Stage */
    /* Return the instruction to issue. Bubble if none. */
    /** Can this instruction be issued.  Are any of its source registers
     *  due to be written by other marked-up instructions in flight */
    virtual Cva6DynInstPtr getIssueInst(bool &is_over_serialise,
      bool &is_raw, Cva6DynInstPtr &producer, bool &is_waw);

    /** Issue the instruction (notify instruction is issued) */
    virtual void issueInst(Cva6DynInstPtr inst);

    bool isInstInFu(Cva6DynInstPtr inst){
      return !inst->execute_completed && inst->issue_completed;
    }
    /** Notify scoreboard functional unit finished */
    void completeInst(Cva6DynInstPtr inst);

    /** Return the instruction to commit. Bubble is none. */
    Cva6DynInstPtr getCommitInst(size_t index=0);

    /** Commit the instruction */
    void pre_commit(Cva6DynInstPtr& inst);
    virtual void commitInst(Cva6DynInstPtr& inst);

    /** Tick the scoreboard: evaluate flip flops*/
    void tick();
    /** Flush issue queue and clear down all the reg dependencies */
    virtual void flush();

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
    bool isUnissedStoreBefore(Cva6DynInstPtr inst_in);
    void dump();
};


class ScoreboardO3 : public Scoreboard
{
  public:
  ScoreboardO3(const std::string &name, Cva6CPU &cpu_, uint64_t size,
        ForwardInstDataPopIntf& inp_, uint64_t sqssize) :
        Scoreboard(name, cpu_, size, inp_),
        iq(name + ".iq"),
        max_inflight_stores(sqssize) { }

  Cva6DynInstChunk iq; // The IQ of the scheduler
  uint64_t inflight_stores = 0;
  uint64_t max_inflight_stores;

  std::deque<Cva6DynInstPtr> store_order;

  /* Check IRO, FUs and MDP deps*/
  bool isReady(Cva6DynInstPtr& inst, bool &is_raw,
    Cva6DynInstPtr &producer, bool &is_waw, bool &is_ss);

  Cva6DynInstPtr getIssueInst(
      bool &is_os,
      bool &is_raw,
      Cva6DynInstPtr &producer,
      bool &is_waw
  ) override {
    /* First of all try to fill IQ */
    /* Dispatch */
    while (inp.canPop() && /* Instructions ready to be scheduled */
          iq.size() < nr_entries && /* Renaning space in IQ */
          !is_serialise_inflight && /* Wait serialisation drain */
          inflight_stores < max_inflight_stores
    ){
      Cva6DynInstPtr inst = inp.pop();
      inflight_stores += !inst->isFault() && inst->staticInst->isStore();
      if (!inst->isFault() && inst->staticInst->isStore()){
        store_order.push_back(inst);
      }
      iq.push(inst); /* Fill the IQ */
      onInsert(inst); /* Markup rd buzy */
    }
    /* Try to find ready candidate */
    is_os = false; // We cannot issues instructions upon a serialization
    bool is_ss = false; /* is store serialise */
    for (auto inst: iq){
      if (isReady(inst, is_raw, producer, is_waw, is_ss)){
        DPRINTF(Cva6Scoreboard, "IQ %s Ready\n", *inst);
        return inst;
      } else {
        DPRINTF(Cva6Scoreboard, "IQ %s %s\n", *inst, is_raw ? "[RaW]" : "");
      }
    }
    return Cva6DynInst::bubble();
  }

  void issueInst(Cva6DynInstPtr inst) override {
    /* Remove inst from the IQ */
    iq.erase(inst);
    if (!inst->isFault() && inst->staticInst->isStore()){
      assert(inst == store_order.front());
      store_order.pop_front();
    }
    /* complete the issue */
    completeIssueInst(inst);
  }

  void commitInst(Cva6DynInstPtr& inst) override {
    inflight_stores -= !inst->isFault() && inst->staticInst->isStore();
    Scoreboard::commitInst(inst);
  }

  void flush() override {
    iq.flush();
    store_order.clear();
    inflight_stores = 0;
    Scoreboard::flush();
  }
};

} // namespace cva6
} // namespace gem5
