/**
 * @file pipeline.hh
 * @author Pierre Ravenel (pravenel@kalray.eu)
 * @brief
 * @version 0.1
 * @date 2023-05-25
 *
 */

#pragma once

#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/func_unit.hh"
#include "cpu/cva6/lambda.hh"
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

/**
 * A simple memory order checker
 * > Note that all stores must be issued in program order !
 * */
class MemOrderChecker : public Named
{
  class Table : public Named
  {
    using data_t = std::pair<uint64_t /*id*/, uint64_t /*pc*/>;

    data_t inst2dat(Cva6DynInstPtr &inst){
      return {inst->id.fetchSeqNum, inst->pc->instAddr()};
    }

    std::map<uint64_t /*Addr*/, data_t> lsidt; /* LastStoreID T*/
    public:
    Table(std::string name) : Named(name) {}

    void markStore(Cva6DynInstPtr &inst){
      assert(inst->dreq);
      lsidt[inst->dreq->getDWPaddr()] = inst2dat(inst);
      DPRINTF(Cva6MDP, "MDPCW [%lx] <- %d\n", inst->dreq->getDWPaddr(),
        inst->id.fetchSeqNum);

    }
    uint64_t checkLoad(Cva6DynInstPtr &inst){
      assert(inst->dreq);
      DPRINTF(Cva6MDP, "MDPCR [%lx] <- %d\n", inst->dreq->getDWPaddr(),
        lsidt[inst->dreq->getDWPaddr()].first);
      return lsidt[inst->dreq->getDWPaddr()].first;
    }

    data_t operator[](Cva6DynInstPtr &inst){
      return lsidt[inst->dreq->getDWPaddr()];
    }
  };

  public: /* TODO private */
  Table issue_table;
  Table commit_table;

  public:

  struct Stats : public statistics::Group
  {
    statistics::Scalar req;
    statistics::Scalar miss;

    Stats(Cva6CPU &cpu) :
      statistics::Group(&cpu, "mdpc"),
      ADD_STAT(req, ""),
      ADD_STAT(miss, "")
    { }
  } stats;

  MemOrderChecker(const std::string &name,
                  Cva6CPU &cpu_) : Named(name),
                  issue_table("TI"),
                  commit_table("TC"),
                  stats(cpu_) {}

  void issue(Cva6DynInstPtr &inst){
    if (!inst->isFault() && inst->staticInst->isStore()){
      issue_table.markStore(inst);
    }
    if (!inst->isFault() && inst->staticInst->isLoad()){
      inst->last_store_id = issue_table.checkLoad(inst);
    }
  }

  /**
   * Return true when a memory hazard append
   */
  bool commit(Cva6DynInstPtr &inst){
    if (!inst->isFault() && inst->staticInst->isStore()){
      commit_table.markStore(inst);
    }
    if (!inst->isFault() && inst->staticInst->isLoad()){
      return inst->last_store_id != commit_table.checkLoad(inst);
    }
    return 0;
  }

  bool isViolation(Cva6DynInstPtr &inst, uint64_t &pcstore){
    if (inst->isFault() || !inst->staticInst->isLoad()){
      return false;
    }
    uint64_t refid = commit_table[inst].first;
    pcstore = commit_table[inst].second;
    bool is_violation = inst->last_store_id < refid;
    /* Use != for perfect serialisation => No skip InFLight stores */
    DPRINTF(Cva6MDP, "Violation [%d] : %d != %d\n", is_violation,
      inst->last_store_id, refid);
    stats.req += 1;
    stats.miss += is_violation;
    return is_violation;
  }

  void flush(){
    /* Is there something to flush ? */
    /* TODO: the speculative table must be corrupted ! copy on flush ?*/
  }
};

/* Simple Direct Map High Confidence predictor for
 * Indirects branchs
 */
struct HCPred
{
  struct pred_entry_t
  {
    uint64_t tag = 0;
    uint64_t cpt;
  };

  #define RBHPSIZE_LOG 8
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

  void commit(Cva6DynInstPtr& inst){
      if (inst->isFault() || !inst->staticInst->isControl()){
          return;
      }
      if (!inst->staticInst->isUncondCtrl()){
          return;
      }
      if (inst->staticInst->isReturn()){
          return;
      }
      if (inst->staticInst->isDirectCtrl()){
        assert(!inst->isASquash());
        return;
      }
      bool squashed = inst->isASquash();
      uint64_t idx = addr2idx(inst->pc->instAddr());
      // uint64_t newtag = addr2tag(inst->pc->instAddr());
      // bool reset = squashed ||
      //              pred_array[idx].tag != newtag;
      // pred_array[idx].cpt = reset ? 0 : pred_array[idx].cpt+1;
      // pred_array[idx].tag = newtag;
      if (squashed){
          pred_array[idx].cpt = 0;
      } else if (!pred_array[idx].cpt) {
          pred_array[idx].cpt = (rand() % 64 == 0);
      }
  }
};


/** The constructed pipeline. */
class Pipeline : public Ticked
{
  protected:
  Cva6CPU &cpu;

  public:
  /** Pipeline shared elements */
  VP &vp;                /** Value predictor for load insts */
  VPDPE &dpe;            /** Delayed Prediction Unit */
  FUPipelines fus;       /** All functional units */
  RegDeadAnayser rda;    /* ? */
  branch_prediction::BPredUnit &bp; /* the main BP */
  HCPred hcpred;         /** Part of the Bpred for HC */

  /* New components */
  SA sa;
  IssueUnit iq;
  Cva6DynInstChunk rob;
  BlockCommit bc;

  MemOrderChecker mdpc;

  /* Plugins */
  Plugins plugins;

  protected:
  /** Pipeline registers */
  Latch<ForwardLineData> f1ToF2;      /* fetched line */
  ForwardInstData        f2ToD;       /* final insts FIFO */
  ForwardInstData        dToIssue;    /* final insts FIFO */
  ForwardInstData        IssueToE;    /* instructions to execute */

  BranchData f2ToF1_nff;              /* F2->F1 prediction */
  BranchData resolved_branch;         /* EX->all stream update */

  /** Pipeline stages */
  Execute execute;
  Issue issue;
  Decode decode;
  Fetch2 fetch2;
  Fetch1 fetch1;

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
      vp(*vpinit(p.vpType, cpu.name() + ".vp", cpu, p.vpSize)),
      dpe(*new VPDPE(cpu.name() + ".dpe", cpu, p, vp)),
      fus(cpu.name() + ".fus", cpu, p),
      rda(cpu.name(), cpu_, p),
      bp(*p.branchPred),
      sa(cpu.name() + ".sa", cpu, p),
      iq(cpu.name() + ".iq", cpu, p, fus),
      rob(cpu.name() + ".rob"),
      bc(cpu),
      mdpc(cpu.name() + "mdpc", cpu),
      plugins(cpu.name(), cpu, p),
      f1ToF2(cpu.name() + ".f1ToF2", "lines"),
      f2ToD(p.issueWidth),
      dToIssue(p.issueWidth),
      IssueToE(p.issueWidth),
      f2ToF1_nff(),
      resolved_branch(),
      execute (cpu.name() + ".execute", cpu, p,
              IssueToE,
              resolved_branch, // Ex -> Commit and Commit -> Ex
              fus,
              dpe,
              bc),
      issue   (cpu.name() + ".issue", cpu, p,
              dToIssue,
              resolved_branch,
              IssueToE,                   // issue -> exe
              fus,
              dpe),
      decode  (cpu.name() + ".decode", cpu, p,
              f2ToD,
              resolved_branch,
              dToIssue,
              dpe),
      fetch2  (cpu.name() + ".fetch2", cpu, p,
              f1ToF2.output(),
              resolved_branch,
              f2ToF1_nff,
              f2ToD,
              dpe),
      fetch1  (cpu.name() + ".fetch1", cpu, p,
              resolved_branch,
              f1ToF2.input(),
              f2ToF1_nff,
              fetch2.inputBuffer),
      stats(cpu)
  { }


  public:
    /** Wake up the Fetch unit after quiesce wakeup */
    void wakeupFetch() {
      fetch1.wakeupFetch();
      this->start();
    }

    /** Try to drain the CPU */
    bool drain() { return false; };

    void drainResume() { ; };

    /** Test to see if the CPU is drained */
    bool isDrained() { return false; };

    /** A custom evaluate allows report in the right place (between
     *  stages and pipeline advance) */
    void evaluate() override;

    /** Return the IcachePort belonging to Fetch1 for the CPU */
    Cva6CPU::Cva6CPUPort &getInstPort() { return fetch1.getIcachePort(); }

    bool isCommitInst(Cva6DynInstPtr inst){
      assert(rob.size());
      return rob.front() == inst;
    }

};

} // namespace cva6
} // namespace gem5
