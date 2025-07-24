/**
 * @file pipeline.hh
 * @author Pierre Ravenel (pravenel@kalray.eu)
 * @brief The full pipeline
 * @version 0.1
 * @date 2023-05-25
 *
 */

#pragma once

#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/func_unit.hh"
#include "cpu/cva6/lambda.hh"
#include "cpu/cva6/mdpc.hh"
#include "cpu/cva6/plugin.hh"
#include "cpu/cva6/scheduler_handler.hh"
#include "cpu/cva6/stage_decode.hh"
#include "cpu/cva6/stage_execute.hh"
#include "cpu/cva6/stage_fetch1.hh"
#include "cpu/cva6/stage_fetch2.hh"
#include "cpu/cva6/stage_issue.hh"
#include "cpu/cva6/vp.hh"
#include "debug/Cva6MDP.hh"
#include "params/BaseCva6CPU.hh"
#include "sim/ticked_object.hh"

uint64_t tictac();

namespace gem5 {
namespace cva6 {

/* Simple Direct Map High Confidence predictor for
 * Indirects branchs
 */
struct HCPred
{
  struct pred_entry_t
  {
    uint64_t tag = 0;
    uint64_t cpt = 0;
  };

  #define RBHPSIZE_LOG 10
  #define RBHPSIZE  (1 << RBHPSIZE_LOG)
  pred_entry_t pred_array[RBHPSIZE];

  uint64_t addr2idx(uint64_t addr){
    uint64_t res = 0;
    while (addr){
      res ^= addr;
      addr >>= RBHPSIZE_LOG;
    }
    return res % RBHPSIZE;
  }

  uint64_t addr2tag(uint64_t addr){ // 8 bits tag
    return addr >> 2 & ((1 << 8) - 1);
  }

  bool predictIsHC(Cva6DynInstPtr& inst){
      uint64_t idx = addr2idx(inst->pc->instAddr());
      // inst->isHighConf = !(pred_array[idx].cpt < 64 ||
      //               pred_array[idx].tag != addr2tag(inst->pc->instAddr()));
      return pred_array[idx].cpt;
  }

  void violation(Cva6DynInstPtr& inst){
    uint64_t idx = addr2idx(inst->pc->instAddr());
    pred_array[idx].cpt = 0;
  }

  void confidence(Cva6DynInstPtr& inst){
    uint64_t idx = addr2idx(inst->pc->instAddr());
    // uint64_t newtag = addr2tag(inst->pc->instAddr());
    // bool reset = squashed ||
    //              pred_array[idx].tag != newtag;
    // pred_array[idx].cpt = reset ? 0 : pred_array[idx].cpt+1;
    // pred_array[idx].tag = newtag;
    if (!pred_array[idx].cpt) {
        pred_array[idx].cpt = (rand() % 64 == 0);
    }
  }

  void commit(Cva6DynInstPtr& inst){}
};


/** The constructed pipeline. */
class Pipeline : public Ticked
{
  using BPredUnit = branch_prediction::BPredUnit;
  protected:
  Cva6CPU &cpu;

  public:
  /** Pipeline shared custom elements */
  VP &vp;                             /* Load Value predictor */
  VPDPE &dpe;                         /* Delayed Prediction Unit */
  RegDeadAnayser rda;                 /* REG_DEAD annotation */
  HCPred hcpred;                      /* Part of the Bpred for HC */
  SA sa;                              /* Handle prescheduling */
  BlockCommit bc;                     /* Handle early commit */
  /* Base components */
  BPredUnit &bp;                      /* the main BP */
  FUPipelines fus;                    /* All functional units */
  IssueUnit iq;                       /* The schedule/IQ */
  Cva6DynInstChunk rob;               /* The reorder buffer */
  StoreSet<Cva6DynInstPtr> mdp;       /* MDP predictor */
  MemOrderChecker mdpc;               /* MDP checker */
  /* Misc */
  Plugins plugins;                    /* Plugins */

  protected:
  /** Forward pipeline registers */
  ForwardLineDataReg     f1ToF2;      /* fetched line */
  ForwardInstData        f2ToD;       /* final insts FIFO */
  ForwardInstData        dToIssue;    /* final insts FIFO */
  ForwardInstData        IssueToE;    /* instructions to execute */
  /** Backward pipeline registers */
  BranchData f2ToF1_nff;              /* F2->F1 prediction */
  BranchData resolved_branch;         /* EX->all stream update */
  /** Pipeline stages */
  Execute execute;                    /* EX, WB and Commit */
  Issue issue;                        /* Dispatch and Issue */
  Decode decode;                      /* Decode */
  Fetch2 fetch2;                      /* PC + Fetch with F1*/
  Fetch1 fetch1;                      /* */

  public:
  struct Stats : public statistics::Group
  {
    statistics::Scalar systemhus;
    statistics::Scalar exhus;
    statistics::Scalar ishus;
    statistics::Scalar dehus;
    statistics::Scalar f2hus;
    statistics::Scalar f1hus;

    statistics::Scalar exfus;
    statistics::Scalar expop;
    statistics::Scalar excommit;

    Stats(Cva6CPU &cpu) :
      statistics::Group(&cpu, "pipeline"),
      ADD_STAT(systemhus, ""),
      ADD_STAT(exhus, ""),
      ADD_STAT(ishus, ""),
      ADD_STAT(dehus, ""),
      ADD_STAT(f2hus, ""),
      ADD_STAT(f1hus, ""),

      ADD_STAT(exfus, ""),
      ADD_STAT(expop, ""),
      ADD_STAT(excommit, "")
    { }
  } stats;

  public:

  Pipeline(Cva6CPU &cpu_, const BaseCva6CPUParams &p) :
      Ticked(cpu_, &(cpu_.BaseCPU::baseStats.numCycles)),
      cpu(cpu_),
      /** Pipeline shared custom elements */
      vp(*vpinit(p.vpType, cpu.name() + ".vp", cpu, p.vpSize)),
      dpe(*new VPDPE(cpu.name() + ".dpe", cpu, p, vp)),
      rda(cpu.name(), cpu_, p),
      sa(cpu.name() + ".sa", cpu, p),
      bc(cpu),
      /* Base components */
      bp(*p.branchPred),
      fus(cpu.name() + ".fus", cpu, p),
      iq(cpu.name() + ".iq", cpu, p, (ForwardInstDataPopIntf&)sa, fus),
      rob(cpu.name() + ".rob", p.numROBEntries),
      mdp(1024),
      mdpc(cpu.name() + "mdpc", cpu),
      /* Misc */
      plugins(cpu.name(), cpu, p),
      /** Forward pipeline registers */
      f2ToD(p.issueWidth),
      dToIssue(p.issueWidth),
      IssueToE(p.issueWidth),
      /** Backward pipeline registers */
      f2ToF1_nff(),
      resolved_branch(),
      /** Pipeline stages */
      execute (cpu.name() + ".execute", cpu, p,
              IssueToE, resolved_branch),
      issue   (cpu.name() + ".issue", cpu, p,
              dToIssue,
              resolved_branch,
              IssueToE,
              fus,
              dpe),
      decode  (cpu.name() + ".decode", cpu, p,
              f2ToD,
              resolved_branch,
              dToIssue),
      fetch2  (cpu.name() + ".fetch2", cpu, p,
              f1ToF2,
              f2ToD,
              resolved_branch,
              f2ToF1_nff,
              dpe),
      fetch1  (cpu.name() + ".fetch1", cpu, p,
              resolved_branch,
              f1ToF2,
              f2ToF1_nff),
      stats(cpu)
  { }


  public:
    /** Wake up the Fetch unit after quiesce wakeup */
    void wakeupFetch() {
      fetch1.wakeupFetch();
      this->start();
    }

    /* The main evaluate function */
    void evaluate() override;
    bool drain() { return true; }

    /** Return the IcachePort belonging to Fetch1 for the CPU */
    Cva6CPU::Cva6CPUPort &getInstPort() { return fetch1.getIcachePort(); }

    bool isCommitInst(Cva6DynInstPtr inst){
      assert(rob.size());
      return rob.front() == inst;
    }

    size_t getNbinflightStoresInRob(){
      size_t cnt = 0;
      for (Cva6DynInstPtr& inst: rob){
        cnt += !inst->isFault() && inst->staticInst->isMemRef() &&
           !inst->staticInst->isLoad();
      }
      return cnt;
    }

    void flushfrom(Cva6DynInstPtr inst, BranchData& branch){
      // Post Backend
      cpu.pipeline->bc.flush(); // Clear inflights pre-committed

      // Backend
      execute.flushfrom(inst); // Flush fus + inp
      issue.flushfrom(inst); // Flush sb + inp

      cpu.pipeline->sa.flushfrom(inst);
      cpu.pipeline->dpe.flushfrom(inst);

      // Frontend
      decode.flush(); // Cannot flush at arbitrary position
      fetch2.flush();
      fetch1.flush();

      cpu.pipeline->mdp.flush();

      /* Squash BP */
      assert(branch.need_squash);
      if (branch.is_predicted) {
          cpu.pipeline->bp.squash(branch.num,
              *branch.target, branch.actually_taken, 0);
      } else {
          cpu.pipeline->bp.squash(branch.num, 0);
      }
      /* And set new stream to fetch ! */
      fetch1.changeStream(branch);

      /* Do the ROB at last bc it is used by other components
       to performs flush */
      cpu.pipeline->rob.flushfrom(inst);
    }
};

} // namespace cva6
} // namespace gem5
