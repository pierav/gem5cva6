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

      IssueStats(const std::string &name, BaseCPU &cpu,
        const BaseCva6CPUParams &params) :
        statistics::Group(&cpu, name.c_str()),
        ADD_STAT(numIssued, statistics::units::Count::get(),
          "Number of insts issued each cycle"),
        ADD_STAT(typeIssued, statistics::units::Count::get(),
          "Number of instructions issued per FU type"),
        ADD_STAT(issue_stall_front, statistics::units::Count::get(),
                 "Frontend stalls issue"),
        ADD_STAT(issue_stall_lambdaorder, "Issue stall : lambda order"),
        ADD_STAT(issue_stall_iro, statistics::units::Count::get(),
                 "Issue read operands stall"),
        ADD_STAT(issue_stall_fu, statistics::units::Count::get(),
                 "Issue functional unit stall"),
        ADD_STAT(issue_pass, statistics::units::Count::get(),
                 "Nothing stall issue"),
         ADD_STAT(issue_stall_raw, statistics::units::Count::get(),
                "Delta Cycles between sb enter and issue"){
        numIssued
          .init(0,params.issueWidth,1)
          .flags(statistics::pdf);

        typeIssued
          .init(1, enums::Num_OpClass)
          .flags(statistics::total | statistics::pdf | statistics::dist);
        typeIssued.ysubnames(enums::OpClassStrings);

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

    private:
    uint64_t lambda_ready = 0;

    public:
    bool canPush(){
      return scoreboard.canPush();
    }

    void push(Cva6DynInstPtr inst){
      if (inst->l_data.is_predicted_last){
        lambda_ready += 1;
      }
      inst->stage_issue_enter = true;
      scoreboard.pushInst(inst);
    }

#if 0
    bool isCommitLambdaReady(){
      assert(scoreboard.getIssueQueue().size());
      assert(scoreboard.getIssueQueue().front()->l_data.is_predicted_first);
      for (Cva6DynInstPtr inst: scoreboard.getIssueQueue()){
        if (inst->isFault() ||
           inst->staticInst->isNonSpeculative() ||
           inst->staticInst->opClass() == No_OpClass){
          return true;
        }
        if (!inst->execute_completed){
          break;
        }
        if (inst->l_data.is_predicted_last){
          return true;
        }
      }
      return false;
    }

    bool commitLambda(){
      Fault fault = NoFault;
      bool misspred = false;

      BinaryLambdaRegFile rf;
      /* The destination register to maintain alive */
      uint64_t rd_val = 0xdeadbeef;
      for (Cva6DynInstPtr inst: scoreboard.getIssueQueue()){
        /* Not a lamdable inst */
        if (inst->isFault() || /* Fault contains store not silent */
           inst->staticInst->isNonSpeculative() ||
           inst->staticInst->opClass() == No_OpClass){
          misspred = true;
          break;
        }
        /* updates rd */
        if (inst->l_data.lambda.rd != 0 && inst->numDstRegs()){
          if (id2i(inst->dstRegIdx(0)) == inst->l_data.lambda.rd){
            rd_val = inst->getDstRegOperand(0);
          }
        }

        fatal_if(!inst->execute_completed, "Must be exec");
        if (inst->l_data.is_predicted_last){
          misspred = !inst->l_data.do_check_pc_next(inst->pc_next->instAddr());
          misspred |= rd_val != inst->l_data.lambda.rd_val;
          break;
        }
      }
      if (misspred){
        /* Let the execute stage decide to flush or replay */
      } else {}
        /* Let the commit stage commit instructions */
      }
      return misspred;
#endif

    void evaluate();

    void execute(){
      for (Cva6DynInstPtr inst: scoreboard.getIssueQueue()){
        if (scoreboard.isInstInFu(inst)){
          // For all instructions in FUs try to complete the execution
          if (fus.canPop(inst)){
            fus.pop(inst);                  /* Compute FU and pop */
            inst->executeComplete();        /* Complete FU result */
            scoreboard.completeInst(inst);  /* Notify scoreboard */
            // TODO commit & flush ???
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

    Cva6DynInstPtr getHeadInst(){
      return scoreboard.getHeadInst();
    }

    void commit(Cva6DynInstPtr inst){
      scoreboard.commitInst(inst);
    }

    void flush(){
      scoreboard.flush();
    }

    bool canInterrupts(){
      Cva6DynInstPtr inst = scoreboard.getHeadInst();

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

    /* The 2 IQ have to synchronize ! */
    BinaryRegisterFile rfsynchro;
    bool robGetRegFunctionnal(uint64_t pos, RegId reg_src, RegVal &fwval);
    bool isLamdbaOrderOk(Cva6DynInstPtr inst);
    bool allowLambdaIq(){
      #if 0
      return scoreboard.getHeadInst()->l_data.is_uop_lambda_pred;
      #endif
      return true;
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
