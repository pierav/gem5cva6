/**
 * @file
 *
 *  The dynamic instruction and instruction/line id (sequence numbers)
 *  definition for Cva6.  A spirited attempt is made here to not carry too
 *  much on this structure.
 */

#pragma once

#include <iostream>

#include "arch/generic/decoder.hh"
#include "arch/generic/isa.hh"
#include "base/named.hh"
#include "base/refcnt.hh"
#include "base/types.hh"
#include "cpu/base.hh"
#include "cpu/cva6/buffers.hh"
#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/exec_context_static.hh"
#include "cpu/cva6/lambda_types.hh"
#include "cpu/cva6/minicache.hh"
#include "cpu/cva6/vp.hh"
#include "cpu/inst_seq.hh"
#include "cpu/static_inst.hh"
#include "cpu/timing_expr.hh"
#include "sim/faults.hh"
#include "sim/insttracer.hh"

namespace gem5 {
namespace cva6 {

class Cva6DynInst;

/** Cva6DynInsts are currently reference counted. */
typedef RefCountingPtr<Cva6DynInst> Cva6DynInstPtr;

/** Id for lines and instructions.  */
class InstId
{
  public:
    /** First sequence numbers to use in initialisation of the pipeline and
     *  to be expected on the first line/instruction issued */
    static const InstSeqNum firstFetchSeqNum = 1;
    static const InstSeqNum firstExecSeqNum = 1;

  public:


    /** Fetch sequence number.  This is 0 for bubbles and an ascending
     *  sequence for the stream of all fetched instructions */
    InstSeqNum fetchSeqNum;

    /** 'Execute' sequence number.  These are assigned after micro-op
     *  decomposition and form an ascending sequence (starting with 1) for
     *  post-micro-op decomposed instructions. */
    InstSeqNum execSeqNum;

  public:
    /** Very boring default constructor */
    InstId(
        InstSeqNum fetch_seq_num = 0,
        InstSeqNum exec_seq_num = 0) :
        fetchSeqNum(fetch_seq_num),
        execSeqNum(exec_seq_num)
    { }

  public:
    /* Equal if set sequence number matches */
    bool
    operator== (const InstId &rhs)
    {
        bool ret = (
            fetchSeqNum == rhs.fetchSeqNum &&
            execSeqNum == rhs.execSeqNum);

        return ret;
    }
};

/** Print this id in the usual slash-separated format */
std::ostream &operator <<(std::ostream &os, const InstId &id);

class Cva6DynInst;

/** Dynamic instruction for Cva6.
 *  Has two separate notions of sequence number for pre/post-micro-op
 *  decomposition: fetchSeqNum and execSeqNum */
class Cva6DynInst : public RefCounted
{
  private:

    /* Pointer back to cpu */
    Cva6CPU *cpu = NULL;

    /** A prototypical bubble instruction.  You must call Cva6DynInst::init
     *  to initialise this */
    static Cva6DynInstPtr bubbleInst;

  public:
    const StaticInstPtr staticInst = nullStaticInstPtr;

    InstId id;
    bool isAfterOrEqual(Cva6DynInstPtr inst_){
      assert(inst_);
      assert(!isBubble());
      if (inst_->isBubble()){
        return true;
      }
      return id.fetchSeqNum >= inst_->id.fetchSeqNum;
    }
    /** Trace information for this instruction's execution */
    trace::InstRecord *traceData = nullptr;

    /** The fetch address of this instruction */
    std::unique_ptr<PCStateBase> pc;

  protected:
    /** This is actually a fault masquerading as an instruction */
    Fault fault = NoFault;    // Fault from frontend
    Fault fault_ex = NoFault; // Fault from backend

  public:
    /** Tried to predict the destination of this inst (if a control
     *  instruction or a sys call) */
    bool triedToPredict = false;

    /** This instruction was predicted to change control flow */
    bool predictedTaken = false;

    /** Predicted branch target */
    std::unique_ptr<PCStateBase> predictedTarget;

    /** FU this instruction is issued to */
    unsigned int fuIndex = 0;

    /** Flag controlling conditional execution of the instruction */
    bool predicate = true;

    /** Flag controlling conditional execution of the memory access associated
     *  with the instruction (only meaningful for loads/stores) */
    bool memAccPredicate = true;


    bool stage_decode_enter = false;
    bool stage_issue_enter = false;

    int cycle_from_issue(){
      if (stage_issue_enter){
        assert(stage_decode_enter);
        return 0;
      }
      if (stage_decode_enter){
        return 1;
      }
      return 2;
    }

    /************ Decode stage ***********/
    // All static informations from decoded insts.
    ExecContextStaticData static_data;

    /************ Issue stage ************/
    uint64_t issue_start_ts = 0;
    uint64_t issue_ts = 0;
    bool issue_completed = false;
    /** Source registers values */
    uint64_t reg_src_val[3] = { 0 }; // TODO Generic N
    bool reg_src_val_valid[3] = { 0 }; // For assertions
    /** memory request generated when load/store */
    DTLBRequestPtr dreq = nullptr;


    /************ Execute stage ************/
    bool execute_completed = false;
    /** Destination registers values */
    uint64_t reg_dst_val[2] = { 0 }; // TODO Generic
    bool reg_dst_val_valid[2] = { 0 };
    // True when a valid reg is overwrite with a different value
    bool reg_dst_overwrite_invalid = false;
    /** Next pc */
    std::unique_ptr<PCStateBase> pc_next; // Next PC
    bool pc_next_taken = false; // Is next pc taken
    /** CSR write */
    std::map<int, RegVal> ex_csrs; /* OUTDATED */
    /** LSU */
    /* Minicache */
    mc_inst_data_t mc_data;
    /** Value prediction */
    vp_inst_metadata_t vp_data;

    /************ Commit ******************/
    bool commit_completed = false; // Used by LSU store buffer
    struct exec_data_t
    {
      bool is_silent_store = true;
      bool is_const_load = true;
      bool is_reg_dead[3] = { false };
      uint8_t pmode;
    } exec_data;

    /************ MetaData ******************/
    lambda_inst_metadata_t l_data;

  public:
    Cva6DynInst() {
      assert(isBubble());
    }

    Cva6DynInst(
      Cva6CPU *cpu_,
      StaticInstPtr si,
      std::unique_ptr<PCStateBase> *pc_) :
      cpu(cpu_),
      staticInst(si)
    {
      set(pc, *pc_);
      assert(id.execSeqNum == 0);
    }

    Cva6DynInst(
      Cva6CPU *cpu_,
      std::unique_ptr<PCStateBase> *pc_,
      Fault fault_) :
      cpu(cpu_),
      fault(fault_)
    {
      set(pc, *pc_);
      // traceData = cpu->getTracer()->getInstRecord(curTick(),
      //   cpu->getContext(),
      //   staticInst, *pc, staticInst);
    }

    //<PC, rs1, rs2, rs3, rd>
    struct inststate_t
    {
      uint64_t regs[5] = {0};
      inststate_t(const Cva6DynInstPtr inst);
      inststate_t() {}
      bool operator==(const struct inststate_t& o) const {
        return memcmp(this, &o, sizeof(inststate_t)) == 0;
      }
      bool operator<(const struct inststate_t& o) const {
        return memcmp(this, &o, sizeof(inststate_t)) < 0;
      }
      size_t dohash() const {
        assert(sizeof(inststate_t) == 8*5);
        uint64_t res = 0;
        for (int i = 0; i < 5; i++){
          res ^= regs[i] << i;
        }
        return res;
      }
      struct Hash
      {
        size_t operator()(const inststate_t p) const {
          return p.dohash();
        }
      };
      struct KeyEqual
      {
        bool operator()(const inststate_t lhs, const inststate_t rhs) const {
          return memcmp(&lhs, &rhs, sizeof(inststate_t)) == 0;
        }
      };
    };

    struct StateHash
    {
      size_t operator()(const Cva6DynInstPtr p) const {
        // assert(p);
        if (!p)
          return 0;
        return inststate_t(p).dohash();
      }
    };

    struct StateKeyEqual
    {
      bool operator()(const Cva6DynInstPtr lhs,
                      const Cva6DynInstPtr rhs) const {
        if (lhs == rhs){
          return true;
        }
        if (!lhs || !rhs){
          return false;
        }
        assert(lhs);
        assert(rhs);
        return inststate_t(lhs) == inststate_t(rhs);
      }
    };


  public:
    /** The BubbleIF interface. */
    bool isBubble() const { return id.fetchSeqNum == 0; }

    /** There is a single bubble inst */
    static Cva6DynInstPtr bubble() { return bubbleInst; }

    void reset(){
      assert(!commit_completed);
      // Reset Issue
      issue_start_ts = 0;
      issue_ts = 0;
      issue_completed = false;
      reg_src_val_valid[0] = false;
      reg_src_val_valid[1] = false;
      reg_src_val_valid[2] = false;
      untrackDreq();

      // Reset Execute
      execute_completed = false;
      reg_dst_val_valid[0] = false;
      reg_dst_val_valid[1] = false;
      reg_dst_overwrite_invalid = false;
      pc_next_taken = false;
      ex_csrs.clear();
      mc_data.reset();
      vp_data.reset();
      setFaultEx(NoFault);

      // Do not reset commit
    }

    /** Is this a fault rather than instruction */
    bool isFault() const {
      // if (fault){
      //   printf("Fault is %s\n", fault->name());
      // }
      // if (fault_ex){
      //   printf("ExFault is %s\n", fault_ex->name());
      // }
      assert(!((fault != NoFault) && (fault_ex != NoFault)));
      return fault != NoFault || fault_ex != NoFault;
    }

    void setFaultFrontend(Fault fault_){
      fault = fault_;
    }

    void setFaultEx(Fault fault_){
      fault_ex = fault_;
    }

    Fault getFault() const {
      if (fault != NoFault){
        return fault;
      } else if (fault_ex != NoFault){
        return fault_ex;
      }
      return fault_ex;
    }

    /** Is this a real instruction */
    bool isInst() const { return !isBubble() && !isFault(); }

    /** Is this a real mem ref instruction */
    bool isMemRef() const { return isInst() && staticInst->isMemRef(); }

    /** Assuming this is not a fault, is this instruction either
     *  a whole instruction or the last microop from a macroop */
    bool isLastOpInInst() const;


    /** *** ExecContext Interface *** */
    uint8_t numSrcRegs() const {
      assert(staticInst);
      return staticInst->numSrcRegs();
    }

    const RegId &srcRegIdx(int i) const { return staticInst->srcRegIdx(i); }

    RegVal getSrcRegOperand(int idx) const {
      assert(staticInst);
      assert(idx < staticInst->numSrcRegs());
      assert(reg_src_val_valid[idx]);
      return reg_src_val[idx];
    }

    void setSrcRegOperand(int idx, RegVal val){
      assert(staticInst);
      assert(idx < staticInst->numSrcRegs());
      reg_src_val_valid[idx] = 1;
      reg_src_val[idx] = val;
    }

    uint8_t numDstRegs() const {
      assert(staticInst);
      return staticInst->numDestRegs();
    }

    RegId dstRegIdx(int idx) const {
      return staticInst->destRegIdx(idx);
    }

    RegVal getDstRegOperand(int idx) const {
      assert(staticInst);
      assert(idx < staticInst->numDestRegs());
      assert(reg_dst_val_valid[idx]);
      return reg_dst_val[idx];
    }

    void setDstRegOperand(int idx, RegVal val){
      assert(staticInst);
      assert(idx < staticInst->numDestRegs());
      if (reg_dst_val_valid[idx] && (reg_dst_val[idx] != val)){
        reg_dst_overwrite_invalid = true;
      }
      reg_dst_val_valid[idx] = 1;
      reg_dst_val[idx] = val;
    }

    const PCStateBase &pcState() {
      // Return fetch PC
      return *pc;
    }

    void pcState(const PCStateBase &val){
      // Set next pc
      if (val == *pc_next){
        // Nothing to do
      } else {
        pc_next_taken = true;
        set(pc_next, &val);
      }
    }

    Fault initiateMemRead(Addr addr, unsigned int size,
                    Request::Flags flags,
                    const std::vector<bool>& byte_enable);

    Fault writeMem(uint8_t *data, unsigned int size, Addr addr,
            Request::Flags flags, uint64_t *res,
            const std::vector<bool>& byte_enable);

    Fault initiateMemAMO(Addr addr, unsigned int size,
                Request::Flags flags, AtomicOpFunctorPtr amo_op);

    RegVal readMiscRegNoEffect(int misc_reg) {
      return cpu->thread->readMiscRegNoEffect(misc_reg);
    }

    RegVal readMiscReg(int misc_reg) {
      // fatal_if(ex_csrs.count(misc_reg), "CSR forward\n");
      return cpu->thread->readMiscReg(misc_reg);
    }

    void setMiscReg(int misc_reg, RegVal val) {
      cpu->thread->setMiscReg(misc_reg, val);
      // ex_csrs[misc_reg] = val;
    }

    ThreadContext *tcBase(){ return cpu->thread->getTC(); }

    /*
     * 3 steps execution:
     *  *INITIATE()*  |     *COMPLETE()*      |   *COMMIT()*
     * Initiate --- [FU] --- Complete --- [Commit] ---         : Memory
     *          --- [FU] --- Execute  --- [Commit] ---         : OP
     *          --- [FU] ---          --- [Commit] --- Execute : CSR
     * INITIATE and COMPLETE support speculative execution
     *
    */
    void executeInitiateStatic();
    void executeInitiate();
    void executeComplete();
    Fault executeCommit(Cva6CPU &cpu, SimpleThread &thread);

    bool readPredicate() const { return predicate; }

    void setPredicate(bool val) { predicate = val; }

    bool readMemAccPredicate() const { return memAccPredicate; }

    void setMemAccPredicate(bool val) { memAccPredicate = val; }

    void untrackDreq(){
      if (!dreq) return; // Nothing to untrack
      dreq->untrack(); // Mark as untracked
      dreq = NULL;
    }

    ~Cva6DynInst();


  std::ostream& basedump(std::ostream &os) const;
};

/** Print a summary of the instruction */
std::ostream &operator <<(std::ostream &os, const Cva6DynInst &inst);

} // namespace cva6
} // namespace gem5
