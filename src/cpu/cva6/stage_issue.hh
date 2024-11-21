/**
 * @file
 *
 * Issue stage
 */


#pragma once

#include <vector>

#include "base/named.hh"
#include "base/types.hh"
#include "cpu/cva6/buffers.hh"
#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/func_unit.hh"
#include "cpu/cva6/pipe_data.hh"
#include "cpu/cva6/scoreboard.hh"
#include "cpu/cva6/vp.hh"
#include "cpu/cva6/vp_dpe.hh"

namespace gem5 {
namespace cva6 {

enum OCS { NoOp = 0, Alu, Fpu, Control, Read, Write, NumOCS };
extern const char* OCSNames[];

inline OCS getOcs(Cva6DynInstPtr &inst){
  if (inst->isFault()){
    return OCS::NoOp;
  }
  if (inst->staticInst->isControl()){
    return OCS::Control;
  }
  if (inst->staticInst->isMemRef()){
    if (inst->staticInst->isLoad()){
      return OCS::Read;
    } else {
      return OCS::Write;
    }
  }
  if (inst->staticInst->isFloating()){
    return OCS::Fpu;
  }
  if (inst->staticInst->isInteger()){
    return OCS::Alu;
  }
  return OCS::NoOp;
}

class IssueUnit : public Named
{
    private:
    /** Pointer back to the containing CPU */
    Cva6CPU &cpu;
    /** Pointer to the execution functional units */
    FUPipelines &fus;
    /** Our scoreboard */
    Scoreboard scoreboard;
    /** Configuration */
    int nb_issue_port;

    struct IssueStats : public statistics::Group
    {
      statistics::Distribution numIssued;
      statistics::Vector2d typeIssued;
      statistics::Scalar issue_stall_front;
      statistics::Scalar issue_stall_lambdaorder;
      statistics::Scalar issue_stall_iro;
      statistics::Scalar issue_stall_fu;
      statistics::Scalar issue_pass;

      statistics::Distribution issue_stall_raw;

      statistics::Vector2d typeStallOnLoad;


      IssueStats(const std::string &name, BaseCPU &cpu,
        const BaseCva6CPUParams &params) :
        statistics::Group(&cpu, name.c_str()),
        ADD_STAT(numIssued, "Number of insts issued each cycle"),
        ADD_STAT(typeIssued, "Number of instructions issued per FU type"),
        ADD_STAT(issue_stall_front, "Frontend stalls issue"),
        ADD_STAT(issue_stall_lambdaorder, "Issue stall : lambda order"),
        ADD_STAT(issue_stall_iro, "Issue read operands stall"),
        ADD_STAT(issue_stall_fu, "Issue functional unit stall"),
        ADD_STAT(issue_pass, "Nothing stall issue"),
        ADD_STAT(issue_stall_raw, "Delta Cycles between sb enter and issue"),
        ADD_STAT(typeStallOnLoad, "typeStallOnLoad"){
        numIssued
          .init(0,params.issueWidth,1)
          .flags(statistics::pdf);

        typeIssued
          .init(1, OCS::NumOCS)
          .flags(statistics::total | statistics::pdf | statistics::dist);
        typeIssued.ysubnames(OCSNames);

        typeStallOnLoad
          .init(1, OCS::NumOCS)
          .flags(statistics::total | statistics::pdf | statistics::dist);
        typeStallOnLoad.ysubnames(OCSNames);

        issue_stall_raw
          .init(0,16,1)
          .flags(statistics::pdf);
      }
    } stats;

    public:
    IssueUnit(const std::string &name_,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params,
      FUPipelines &fus_) :
        Named(name_),
        cpu(cpu_),
        fus(fus_),
        scoreboard(name_ + ".scoreboard", cpu, params.sbSize),
        nb_issue_port(params.issueWidth),
        stats(name_, cpu_, params) {}

    public:
    bool canPush(){
      return scoreboard.canPush();
    }

    void push(Cva6DynInstPtr inst){
      inst->stage_issue_enter = true;
      scoreboard.pushInst(inst);
    }

    void evaluate();

    void execute(){
      for (Cva6DynInstPtr inst: scoreboard.getIssueQueue()){
        if (scoreboard.isInstInFu(inst)){
          // For all instructions in FUs try to complete the execution
          if (fus.canPop(inst)){
            fus.pop(inst);                  /* Compute FU and pop */
            inst->executeComplete();        /* Complete FU result */
            scoreboard.completeInst(inst);  /* Notify scoreboard */
          }
        }
      }
    }

    bool isCommitInst(Cva6DynInstPtr inst){
      return scoreboard.getCommitInst() == inst;
    }

    Cva6DynInstPtr getCommitInst(size_t index){
      return scoreboard.getCommitInst(index);
    }

    void commit(Cva6DynInstPtr inst){
      scoreboard.commitInst(inst);
    }

    void flush(){
      scoreboard.flush();
    }

    bool canInterrupts(){
      Cva6DynInstPtr inst = scoreboard.getCommitInst();

      if (!inst->isBubble()){
        if (inst->isFault()){
          return false;
        }
        if (inst->isMemRef()){
          // if (inst->dreq && inst->dreq->isOutsideCpu()){
          return false;
        }
        if (inst->staticInst->isAtomic() ||
            inst->staticInst->isStoreConditional()) {
          return false;
        }
      }
      return true;
    }

    void dump(){
      scoreboard.dump();
    }

    void tick(){
      scoreboard.tick();
    }
};

/** Issue stage. */
class Issue : public Named
{
  protected:

    /** Input port carrying instructions from Decode */
    ForwardInstData &inp;
    /** Input port carrying resolved branch from Execute */
    BranchData &resolved_branch;
    /** Output port carrying instructions to  execute */
    // ForwardInstData &out;
    /** Pointer back to the containing CPU */
    Cva6CPU &cpu;
  public:

    Issue(const std::string &name_,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params,
      ForwardInstData &inp_,
      BranchData &resolved_branch_,
      ForwardInstData &out_,
      FUPipelines &fus_,
      VPDPE& dpe_):
      Named(name_),
      inp(inp_),
      resolved_branch(resolved_branch_),
      cpu(cpu_) {}

    ~Issue() {}

  public:

    /** Pass on input/buffer data to the output if you can */
    void evaluate();

    /** Flush input and scoreboard. */
    void flush();
};

} // namespace cva6
} // namespace gem5
