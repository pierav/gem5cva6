/**
 * @file
 *
 *  Fetch2 receives lines of data from Fetch1, separates them into
 *  instructions and passes them to Decode
 */

#pragma once

#include "base/named.hh"
#include "cpu/cva6/buffers.hh"
#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/pipe_data.hh"
#include "cpu/cva6/vp_dpe.hh"
#include "cpu/pred/bpred_unit.hh"
#include "params/BaseCva6CPU.hh"

namespace gem5 {
namespace cva6 {

struct UDecoder
{
    /** Pointer back to the containing CPU */
    Cva6CPU &cpu;

    UDecoder(Cva6CPU &cpu_) : cpu(cpu_) {}

    /** True when we're in the process of decomposing a micro-op and
     *  microopPC will be valid.  This is only the case when there isn't
     *  sufficient space in Executes input buffer to take the whole of a
     *  decomposed instruction and some of that instructions micro-ops must
     *  be generated in a later cycle */
    bool inMacroop = false;
    std::unique_ptr<PCStateBase> microopPC;

    /** Source of execSeqNums to number instructions. */
    InstSeqNum execSeqNum = InstId::firstExecSeqNum;

    void flush(){
      inMacroop = false;
    }

   Cva6DynInstPtr decodeInst(Cva6DynInstPtr inst,
     bool &input_finished);

};

/** This stage receives lines of data from Fetch1, separates them into
 *  instructions and passes them to Decode */
class Fetch2 : public Named
{
  protected:
    /** Pointer back to the containing CPU */
    Cva6CPU &cpu;

    /** Input port carrying lines from Fetch1 */
    Latch<ForwardLineData>::Output inp;

    /** Input port carrying branches from Execute.  This is a snoop of the
     *  data provided to F1. */
    BranchData &resolved_branch;

    /** Output carrying predictions back to Fetch1 */
    BranchData &predictionOut;

    /** Output port carrying instructions into Decode */
    ForwardInstDataPushIntf &out;

    /* uOP decoder */
    UDecoder udecoder;

    /* Delayed prediction unit */
    VPDPE &dpe;

  public:
    /* Public so that Pipeline can pass it to Fetch1 */
    InputBuffer<ForwardLineData> inputBuffer;

  protected:
    /** Data members after this line are cycle-to-cycle state */

    struct Fetch2ThreadInfo
    {
      Fetch2ThreadInfo() {}

      Fetch2ThreadInfo(const Fetch2ThreadInfo& other) :
          havePC(other.havePC)
      {
          set(pc, other.pc);
      }

      /** Remembered program counter value.  Between contiguous lines, this
       *  is just updated with advancePC.  For lines following changes of
       *  stream, a new PC must be loaded and havePC be set.
       *  havePC is needed to accomodate instructions which span across
       *  lines meaning that Fetch2 and the decoder need to remember a PC
       *  value and a partially-offered instruction from the previous line */
      std::unique_ptr<PCStateBase> pc;

      /** PC is currently valid.  Initially false, gets set to true when a
       *  change-of-stream line is received and false again when lines are
       *  discarded for any reason */
      bool havePC = false;

      /** Fetch2 is the source of fetch sequence numbers.  These represent the
       *  sequence that instructions were extracted from fetched lines. */
      InstSeqNum fetchSeqNum = InstId::firstFetchSeqNum;
    };

    Fetch2ThreadInfo fetchInfo;

    // struct Fetch2Stats : public statistics::Group
    // {
    //     Fetch2Stats(Cva6CPU *cpu);
    //     /** Stats */
    //     statistics::Scalar intInstructions;
    //     statistics::Scalar fpInstructions;
    //     statistics::Scalar vecInstructions;
    //     statistics::Scalar loadInstructions;
    //     statistics::Scalar storeInstructions;
    //     statistics::Scalar amoInstructions;
    // } stats;

    struct Fetch2Stats : public statistics::Group
    {
      /** Distribution of number of instructions fetched each cycle. */
      statistics::Distribution nisnDist;

      Fetch2Stats(Cva6CPU *cpu) :
        statistics::Group(cpu, "fetch"),
        ADD_STAT(nisnDist, statistics::units::Count::get(),
          "Number of instructions fetched each cycle (Total)"){
        nisnDist
        .init(/* base value */ 0,
          /* last value */ 32,
          /* bucket size */ 1)
        .flags(statistics::pdf);
      }
    } stats;

  protected:
    /** Get a piece of data to work on from the inputBuffer, or 0 if there
     *  is no data. */
    const ForwardLineData *getInput();

    /** Pop an element off the input buffer, if there are any */
    void popInput();

    /** Dump the whole contents of the input buffer.  Useful after a
     *  prediction changes control flow */
    void dumpAllInput();

    /** Update local branch prediction structures from feedback from
     *  Execute. */
    void updateBranchPrediction(const BranchData &branch);

    /** Predicts branches for the given instruction.  Updates the
     *  instruction's predicted... fields and also the branch which
     *  carries the prediction to Fetch1 */
    void predictBranch(Cva6DynInstPtr inst, BranchData &branch);
    bool computeHighConf(Cva6DynInstPtr& inst);

    void output_inst(Cva6DynInstPtr inst);

  public:
    Fetch2(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params,
      Latch<ForwardLineData>::Output inp_,
      BranchData &resolved_branch_,
      BranchData &predictionOut_,
      ForwardInstDataPushIntf &out_,
      VPDPE &dpe_) :
      Named(name),
      cpu(cpu_),
      inp(inp_),
      resolved_branch(resolved_branch_),
      predictionOut(predictionOut_),
      out(out_),
      udecoder(cpu_),
      dpe(dpe_),
      inputBuffer(name + ".inputBuffer", "insts"),
      fetchInfo(),
      stats(&cpu_) { }


  public:
    /** Pass on input/buffer data to the output if you can */
    void evaluate();

    /** Flush the stage */
    void flush();
};

} // namespace cva6
} // namespace gem5
