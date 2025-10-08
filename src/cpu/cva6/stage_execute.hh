/**
 * @file stage_execute.hh
 * @author Pierre Ravenel (pravenel@kalrayinc.com)
 * @brief
 * @version x
 * @date 2023-05-25
 *
 */

#pragma once

#include "base/named.hh"
#include "base/types.hh"
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

/* Update the arch state */
bool commitInst(Cva6CPU& cpu, Cva6DynInstPtr inst);
bool isASquash(Cva6DynInstPtr inst);
BranchData getEffectiveBranch(Cva6DynInstPtr inst);

struct BlockCommit
{
  Cva6CPU& cpu;
  struct Stats : public statistics::Group
  {
    statistics::Scalar lost;
    statistics::Scalar committed;
    statistics::Distribution committed_block_size;
    statistics::Scalar regwrite;
    statistics::Scalar regwriteeff;

    Stats(Cva6CPU &cpu) :
      statistics::Group(&cpu, "BC"),
      ADD_STAT(lost, ""),
      ADD_STAT(committed, ""),
      ADD_STAT(committed_block_size, ""),
      ADD_STAT(regwrite, ""),
      ADD_STAT(regwriteeff, "")
    {
      committed_block_size
        .init(1,128,1)
        .flags(statistics::pdf);
    }
  } stats;

  Cva6DynInstChunk fifo;
  ArchRegFile<char> preg_in_flight;
  ArchRegFile<uint64_t> preg_val;

  BlockCommit(Cva6CPU &cpu_) :
    cpu(cpu_),
    stats(cpu_),
    fifo("rbc") {}

  void clear();
  bool pre_commit(Cva6DynInstPtr& inst);

  void flush(){
    stats.lost += fifo.size();
    clear();
  }

  // Dont trow committed jobs ?
  bool canInterrupts(){ return fifo.empty(); }
  bool empty(){ return fifo.empty(); }
  uint64_t size() { return fifo.size(); }
  void dump();
  bool commitFunctionnal();

};

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


    // Some config
    unsigned int commitWidth;
    bool flushAtExecute = false;
    bool vpFlush;
    bool oracleEarlyCommit = false;

  protected:
    struct ExStats : public statistics::Group
    {
      statistics::Scalar flush;
      statistics::Scalar flush_cond_direct;
      statistics::Scalar flush_cond_indirect;
      statistics::Scalar flush_uncond_direct;
      statistics::Scalar flush_uncond_indirect;
      statistics::Scalar flush_fault;
      statistics::Scalar flush_squashafter;
      statistics::Scalar flush_serialise;
      statistics::Scalar flush_mdp;
      statistics::Scalar flush_vp;
      statistics::Scalar flush_load;

      statistics::Scalar flush_control;
      statistics::Scalar flush_control_drop;
      statistics::Scalar flush_mdp_drop;


      ExStats(Cva6CPU &cpu) :
        statistics::Group(&cpu, "exec"),
        ADD_STAT(flush, ""),
        ADD_STAT(flush_cond_direct, ""),
        ADD_STAT(flush_cond_indirect, ""),
        ADD_STAT(flush_uncond_direct, ""),
        ADD_STAT(flush_uncond_indirect, ""),
        ADD_STAT(flush_fault, ""),
        ADD_STAT(flush_squashafter, ""),
        ADD_STAT(flush_serialise, ""),
        ADD_STAT(flush_mdp, ""),
        ADD_STAT(flush_vp, ""),
        ADD_STAT(flush_load, ""),
        ADD_STAT(flush_control, ""),
        ADD_STAT(flush_control_drop, ""),
        ADD_STAT(flush_mdp_drop, "") {  }
    } stats;
  protected:

    /** Generate Branch data based (into branch) on an observed (or not)
     *  change in PC while executing an instruction.
     *  Also handles branch prediction information within the inst. */
    void tryToBranch(Cva6DynInstPtr inst, BranchData &branch);

    /** Check possible interrupts. */
    bool checkInterrupts();

  public:
    Execute(const std::string &name_,
        Cva6CPU &cpu_,
        const BaseCva6CPUParams &p,
        ForwardInstDataPopIntf &inp_,
        BranchData& resolved_branch_) :
        Named(name_),
        inp(inp_),
        resolved_branch(resolved_branch_),
        cpu(cpu_),
        commitWidth(p.commitWidth),
        flushAtExecute(p.flushAtExecute),
        vpFlush(p.vpFlush),
        oracleEarlyCommit(p.oracleEarlyCommit),
        stats(cpu_)
    { }

    ~Execute() {}

  public:
    /** Pass on input/buffer data to the output if you can */
    void evaluate();

    /** Flush input and FUS */
    void flushfrom(Cva6DynInstPtr inst);
    void do_flush();

};

} // namespace cva6
} // namespace gem5
