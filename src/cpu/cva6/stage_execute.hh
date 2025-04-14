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
#include "cpu/cva6/misc/reg_dead.hh"
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

    /** Pointer to the execution functional units */
    FUPipelines &fus;

    /** Pointer to the value predictor */
    VPDPE &dpe;

    /** Plugins */
    std::list<Plugin*> plugins;

    unsigned int commitWidth;
    bool vpFlush;

    /* Checker */
    InfiniteMemory64 memcheck;

    struct ExStats : public statistics::Group
    {
      statistics::Scalar flush;
      statistics::Scalar flush_cond_direct;
      statistics::Scalar flush_cond_indirect;
      statistics::Scalar flush_uncond_direct;
      statistics::Scalar flush_uncond_indirect;
      statistics::Scalar flush_fault;

      statistics::Scalar ConfMatch;
      statistics::Scalar NoConfNoMatch;
      statistics::Scalar ConfNoMatch;
      statistics::Scalar NoConfMatch;
      ExStats(Cva6CPU &cpu) :
        statistics::Group(&cpu, "exec"),
        ADD_STAT(flush, ""),
        ADD_STAT(flush_cond_direct, ""),
        ADD_STAT(flush_cond_indirect, ""),
        ADD_STAT(flush_uncond_direct, ""),
        ADD_STAT(flush_uncond_indirect, ""),
        ADD_STAT(flush_fault, ""),
        ADD_STAT(ConfMatch, ""),
        ADD_STAT(NoConfNoMatch, ""),
        ADD_STAT(ConfNoMatch, ""),
        ADD_STAT(NoConfMatch, "") {  }
    } stats;
  protected:

    /** Generate Branch data based (into branch) on an observed (or not)
     *  change in PC while executing an instruction.
     *  Also handles branch prediction information within the inst. */
    void tryToBranch(Cva6DynInstPtr inst, Fault fault, BranchData &branch);

    /** Check possible interrupts. */
    bool checkInterrupts();

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
        VPDPE &dpe_) :
        Named(name_),
        inp(inp_),
        resolved_branch(resolved_branch_),
        cpu(cpu_),
        fus(fus_),
        dpe(dpe_),
        commitWidth(params.commitWidth),
        vpFlush(params.vpFlush),
        memcheck("memcheck", cpu_),
        stats(cpu_)
    {
      if (params.plugin_memtrace_path != ""){
        plugins.push_back(new PluginMemtrace(
            name_ + "memtrace", cpu_, params));
      }
      plugins.push_back(new PluginSimpointBar(name_ + "simbar", cpu, params));
      // plugins.push_back(new PluginVPP(name_ + "vpp", cpu, params));
      // plugins.push_back(new PluginMCVP(name_ + "mcvp", cpu, params));
      plugins.push_back(new PluginGoodbadTrap(name_ + "gbt", cpu, params));

      // plugins.push_back(new PluginLambda(name_ + "lambda", cpu, params));
      // plugins.push_back(new PluginMemConst(name_ + "memc", cpu, params));
      // plugins.push_back(new PluginScheduler(name_ + ".sched", cpu, params));
      plugins.push_back(new PluginHMP(name_ + ".hmp", cpu, params));
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
