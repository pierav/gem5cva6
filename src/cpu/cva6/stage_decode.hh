/**
 * @file
 *
 *  Decode collects macro-ops from Fetch2 and splits them into micro-ops
 *  passed to Execute.
 */

#pragma once

#include "base/named.hh"
#include "cpu/cva6/buffers.hh"
#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/pipe_data.hh"
#include "cpu/cva6/vp_dpe.hh"

namespace gem5 {
namespace cva6 {

/* Decode takes instructions from Fetch and decomposes them into
 * micro-ops to feed to Issue.
 */
class Decode : public Named
{
  protected:
    /** Pointer back to the containing CPU */
    Cva6CPU &cpu;
    /** Input port carrying macro instructions from Fetch2 */
    ForwardInstData &inp;
    /** Resolved branch from Execute */
    BranchData &resolved_branch;
    /** Output port carrying micro-op decomposed instructions to Issue */
    ForwardInstData &out;

  protected:
    /** Decode microops */
    Cva6DynInstPtr decodeInst(Cva6DynInstPtr inst, bool &input_finished);
    void flush();

  public:
    Decode(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params,
      ForwardInstData &inp_,
      BranchData &resolved_branch_,
      ForwardInstData &out_):
      Named(name),
      cpu(cpu_),
      inp(inp_),
      resolved_branch(resolved_branch_),
      out(out_)
    { }

  public:
    void evaluate();
};

} // namespace cva6
} // namespace gem5
