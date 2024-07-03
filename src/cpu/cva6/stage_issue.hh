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

/** Issue stage. */
class Issue : public Named
{
  protected:

    /** Input port carrying instructions from Decode */
    ForwardInstData &inp;
    /** Input port carrying resolved branch from Execute */
    BranchData &resolved_branch;
    /** Output port carrying instructions to  execute */
    ForwardInstData &out;

    /** Pointer back to the containing CPU */
    Cva6CPU &cpu;
    /** Pointer to the execution functional units */
    FUPipelines &fus;
    /** Pointer to the scoreboard */
    Scoreboard &scoreboard;
    /** Pointer to the value predictor */
    VPDPE &dpe;

    /** Configuration */
    int nb_issue_port;

    struct IssueStats : public statistics::Group
    {
      statistics::Distribution numIssued;
      statistics::Vector2d typeIssued;

      IssueStats(BaseCPU &cpu, const BaseCva6CPUParams &params) :
        statistics::Group(&cpu, "issue"),
        ADD_STAT(numIssued, statistics::units::Count::get(),
          "Number of insts issued each cycle"),
        ADD_STAT(typeIssued, statistics::units::Count::get(),
          "Number of instructions issued per FU type"){
        numIssued
          .init(0,params.issueWidth,1)
          .flags(statistics::pdf);

        typeIssued
          .init(1, enums::Num_OpClass)
          .flags(statistics::total | statistics::pdf | statistics::dist);
        typeIssued.ysubnames(enums::OpClassStrings);
      }
    } stats;

  public:

    Issue(const std::string &name_,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params,
      ForwardInstData &inp_,
      BranchData &resolved_branch_,
      ForwardInstData &out_,
      FUPipelines &fus_,
      Scoreboard &scoreboard_,
      VPDPE& dpe_):
      Named(name_),
      inp(inp_),
      resolved_branch(resolved_branch_),
      out(out_),
      cpu(cpu_),
      fus(fus_),
      scoreboard(scoreboard_),
      dpe(dpe_),
      nb_issue_port(params.issueWidth),
      stats(cpu_, params) {}

    ~Issue() {}

  public:

    /** Pass on input/buffer data to the output if you can */
    void evaluate();

    /** Flush input and scoreboard. */
    void flush();
};

} // namespace cva6
} // namespace gem5
