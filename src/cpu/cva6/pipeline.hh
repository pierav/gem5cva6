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
#include "cpu/cva6/stage_decode.hh"
#include "cpu/cva6/stage_execute.hh"
#include "cpu/cva6/stage_fetch1.hh"
#include "cpu/cva6/stage_fetch2.hh"
#include "cpu/cva6/stage_issue.hh"
#include "cpu/cva6/vp.hh"
#include "params/BaseCva6CPU.hh"
#include "sim/ticked_object.hh"

namespace gem5 {
namespace cva6 {

enum iq_enum_t { FAST_IQ = 0, LAMBDA_IQ = 1 };

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
    RegDeadAnayser rda;

    /* New components */
    LambdaHandler lh;      /** Lambda handler */
    IssueUnit iq0;
    IssueUnit iq1;
    Cva6DynInstChunk rob;

    IssueUnit &getIq(bool inLambda){ return inLambda ? iq1 : iq0; }

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

  Pipeline(Cva6CPU &cpu_, const BaseCva6CPUParams &p) :
      Ticked(cpu_, &(cpu_.BaseCPU::baseStats.numCycles)),
      cpu(cpu_),
      vp(*vpinit(p.vpType, cpu.name() + ".vp", cpu, p.vpSize)),
      dpe(*new VPDPE(cpu.name() + ".dpe", cpu, p, vp)),
      fus(cpu.name() + ".fus", cpu, p),
      rda(cpu.name(), cpu_, p),
      lh(cpu.name() + ".lh", cpu, p),
      iq0(cpu.name() + ".iq0", cpu, p, fus),
      iq1(cpu.name() + ".iq1", cpu, p, fus),
      rob(cpu.name() + ".rob"),
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
              dpe),
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
              fetch2.inputBuffer)
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

};

} // namespace cva6
} // namespace gem5
