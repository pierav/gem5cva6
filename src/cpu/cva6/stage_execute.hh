/**
 * @file stage_execute.hh
 * @author your name (pravenel@kalray.eu)
 * @brief
 * @version 0.1
 * @date 2023-05-25
 *
 */
#pragma once

#include <vector>

#include "base/named.hh"
#include "base/types.hh"
#include "cpu/cva6/buffers.hh"
#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/func_unit.hh"
#include "cpu/cva6/pipe_data.hh"
#include "cpu/cva6/plugin.hh"
#include "cpu/cva6/scoreboard.hh"
#include "cpu/cva6/vp.hh"
#include "cpu/cva6/vp_dpe.hh"

namespace gem5 {
namespace cva6 {

/** Execute stage. */
class Execute : public Named
{
  protected:
    /** Input port carrying instructions from Issue */
    ForwardInstDataPopIntf &inp;

    /** Input port carrying resolved branch from Execute */
    BranchData& resolved_branch;

    /** Pointer back to the containing CPU */
    Cva6CPU &cpu;

    /** Pointer to the scoreboard */
    Scoreboard &scoreboard;

    /** Pointer to the execution functional units */
    FUPipelines &fus;

    /** Pointer to the value predictor */
    VPDPE &dpe;
    /** Pointer to the minicache */
    Minicache &mc;

    /** Plugins */
    std::list<Plugin*> plugins;

    unsigned int commitWidth;
    bool vpFlush;

  protected:

    /** Generate Branch data based (into branch) on an observed (or not)
     *  change in PC while executing an instruction.
     *  Also handles branch prediction information within the inst. */
    void tryToBranch(Cva6DynInstPtr inst, Fault fault, BranchData &branch);

    /** Check possible interrupts. */
    bool checkInterrupts();

    /** Invoke interrupt. */
    bool takeInterrupt(BranchData &branch);

    /** Do the stats handling and instruction count and PC event events
     *  related to the new instruction/op counts */
    void doInstCommitAccounting(Cva6DynInstPtr inst);

    /** Commit a single instruction. */
    bool commitInst(Cva6DynInstPtr inst, BranchData &branch);

  public:
    Execute(const std::string &name_,
        Cva6CPU &cpu_,
        const BaseCva6CPUParams &params,
        ForwardInstDataPopIntf &inp_,
        BranchData& resolved_branch_,
        FUPipelines &fus_,
        Scoreboard &scoreboard_,
        VPDPE &dpe_,
        Minicache &mc_) :
        Named(name_),
        inp(inp_),
        resolved_branch(resolved_branch_),
        cpu(cpu_),
        scoreboard(scoreboard_),
        fus(fus_),
        dpe(dpe_),
        mc(mc_),
        commitWidth(params.commitWidth),
        vpFlush(params.vpFlush)
    {
      if (params.plugin_memtrace_path != ""){
        plugins.push_back(new PluginMemtrace(
            name_ + "memtrace", cpu_, params));
      }
      plugins.push_back(new PluginSimpointBar(name_ + "simbar", cpu, params));
      plugins.push_back(new PluginVPP(name_ + "vpp", cpu, params));
      plugins.push_back(new PluginMCVP(name_ + "mcvp", cpu, params));
      plugins.push_back(new PluginGoodbadTrap(name_ + "gbt", cpu, params));
      // plugins.push_back(new PluginLambda(name_ + "lambda", cpu, params));
      // plugins.push_back(new PluginMemConst(name_ + "memc", cpu, params));
    }

    ~Execute() {}

  public:
    /** Pass on input/buffer data to the output if you can */
    void evaluate();

    /** Flush input and FUS */
    void flushfrom(Cva6DynInstPtr inst);
    void flush(){
      flushfrom(Cva6DynInst::bubble());
    }

};

} // namespace cva6
} // namespace gem5
