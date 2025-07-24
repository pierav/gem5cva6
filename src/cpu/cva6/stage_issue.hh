/**
 * @file stage_issue.hh
 * @author Pierre Ravenel (pravenel@kalray.eu)
 * @brief
 * @version 0.1
 * @date 2023-05-25
 *
 */

#pragma once

#include <vector>
#include "base/named.hh"
#include "base/types.hh"
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
  assert(inst->staticInst);
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

inline Scoreboard& scoreboardInit(const std::string &name,
        Cva6CPU &cpu,  const BaseCva6CPUParams &p,
        ForwardInstDataPopIntf& inp){
  if (p.sbOoO){
    return *new ScoreboardO3(name + ".sb", cpu, p.sbSize, inp,
      p.lsuSQSWidth);
  } else if (p.sbFSC) {
    return *new ScoreboardFSC(name + ".sb", cpu, p.sbSize, inp);
  } else {
    return *new Scoreboard(name + ".sb", cpu, p.sbSize, inp);
  }
}

class IssueUnit : public Named
{
    private:
    /** Pointer back to the containing CPU */
    Cva6CPU &cpu;
    /** Pointer to the execution functional units */
    FUPipelines &fus;
    /** Our scoreboard */
    Scoreboard& scoreboard;
    /** Configuration */
    int nb_issue_port;
    int loadPerCycle;

    struct IssueStats : public statistics::Group
    {
      statistics::Distribution numIssued;
      statistics::Distribution numIssuedReadFRF;
      statistics::Distribution numIssuedReadARF;

      statistics::Vector2d typeIssued;
      statistics::Scalar issue_stall_front;
      statistics::Scalar issue_stall_serialise;
      statistics::Scalar issue_stall_full;

      statistics::Scalar issue_stall_lambdaorder;
      statistics::Scalar issue_stall_iro;
      statistics::Scalar issue_stall_waw;
      statistics::Scalar issue_stall_fu;
      statistics::Scalar issue_stall_port;
      statistics::Scalar issue_pass;

      statistics::Distribution issue_stall_raw;

      statistics::Vector2d typeStallOnLoad;

      statistics::Vector2d typeStallFU;
      statistics::Vector2d typeStallProducerReg;

      IssueStats(const std::string &name, BaseCPU &cpu,
        const BaseCva6CPUParams &params) :
        statistics::Group(&cpu, name.c_str()),
        ADD_STAT(numIssued, "Number of insts issued each cycle"),
        ADD_STAT(numIssuedReadFRF, "Number of read FRF each cycle"),
        ADD_STAT(numIssuedReadARF, "Number of read ARF each cycle"),
        ADD_STAT(typeIssued, "Number of instructions issued per FU type"),
        ADD_STAT(issue_stall_front, "Frontend stalls issue"),
        ADD_STAT(issue_stall_serialise, "Cycles spent after serialise"),
        ADD_STAT(issue_stall_full, "Scoreboard is full"),
        ADD_STAT(issue_stall_lambdaorder, "Issue stall : lambda order"),
        ADD_STAT(issue_stall_iro, "Issue read operands stall"),
        ADD_STAT(issue_stall_waw, "WaW dependancy"),
        ADD_STAT(issue_stall_fu, "Issue functional unit stall"),
        ADD_STAT(issue_stall_port, "No more issue port"),
        ADD_STAT(issue_pass, "Nothing stall issue"),
        ADD_STAT(issue_stall_raw, "Delta Cycles between sb enter and issue"),
        ADD_STAT(typeStallOnLoad, "typeStallOnLoad"),
        ADD_STAT(typeStallFU, "typeStallFU"),
        ADD_STAT(typeStallProducerReg, "typeStallProducerReg")
        {
        numIssued
          .init(0,params.issueWidth,1)
          .flags(statistics::pdf);
        numIssuedReadARF
          .init(0,params.issueWidth*2,1)
          .flags(statistics::pdf);
        numIssuedReadFRF
          .init(0,params.issueWidth*2,1)
          .flags(statistics::pdf);

        typeIssued
          .init(1, OCS::NumOCS)
          .flags(statistics::total | statistics::pdf | statistics::dist);
        typeIssued.ysubnames(OCSNames);

        typeStallOnLoad
          .init(1, OCS::NumOCS)
          .flags(statistics::total | statistics::pdf | statistics::dist);
        typeStallOnLoad.ysubnames(OCSNames);

        typeStallFU
          .init(1, OCS::NumOCS)
          .flags(statistics::total | statistics::pdf | statistics::dist);
        typeStallFU.ysubnames(OCSNames);

        typeStallProducerReg
          .init(1, OCS::NumOCS)
          .flags(statistics::total | statistics::pdf | statistics::dist);
        typeStallProducerReg.ysubnames(OCSNames);

        issue_stall_raw
          .init(0,16,1)
          .flags(statistics::pdf);
      }
    } stats;

    public:
    IssueUnit(const std::string &name_,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params,
      ForwardInstDataPopIntf& inp,
      FUPipelines &fus_) :
        Named(name_),
        cpu(cpu_),
        fus(fus_),
        scoreboard(scoreboardInit(name_, cpu, params, inp)),
        nb_issue_port(params.issueWidth),
        loadPerCycle(params.loadPerCycle),
        stats(name_, cpu_, params) {}

    public:

    void evaluate();

    void completeInst(Cva6DynInstPtr inst){
      scoreboard.completeInst(inst);
    }

    bool isCommitInst(Cva6DynInstPtr inst){
      return scoreboard.getCommitInst() == inst;
    }
    Cva6DynInstPtr getCommitInst(size_t index){
      return scoreboard.getCommitInst(index);
    }
    void pre_commit(Cva6DynInstPtr inst){
      scoreboard.pre_commit(inst);
    }
    void commit(Cva6DynInstPtr inst){
      scoreboard.commitInst(inst);
    }

    void flushfrom(Cva6DynInstPtr inst){
      scoreboard.flushfrom(inst);
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

    bool markMemoryViolation(Cva6DynInstPtr inst){
      return scoreboard.markMemoryViolation(inst);
    }

    void forwardSpeculativeRegVal(PhysicalReg &reg, RegVal regval){
      scoreboard.forwardSpeculativeRegVal(reg, regval);
    }

    Scoreboard::iq_t &getIssueQueue(){
      return scoreboard.getIssueQueue();
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
    void flushfrom(Cva6DynInstPtr inst);
};

} // namespace cva6
} // namespace gem5
