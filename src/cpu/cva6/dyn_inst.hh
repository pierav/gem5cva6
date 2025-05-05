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

struct PhysicalReg
{
  /* Register type */
  int classValue = 0;         /* If 0 : invalid */
  bool isRenammed = false;    /* Is register renammed */
  uint64_t virt_reg_idx = 0;  /* Virtual reg index */
  uint64_t phys_reg_idx = 0;  /* Physical reg index */
  bool isLastRename = false;  /* Mark the deallocation of the register */
  bool is_reg_dead = false;   /* Is this one dead */
  RegId regid;  /* The gem5 internal register identifier */
  /* Value */
  bool valid = false;         /* Is value valid */
  /* Some metadata */
  bool fromrf = false;        /* Is read from register file */
  bool fromrf_unsafe = false; /* Is read from inflight committed  */
  uint64_t value = 0xdeaddeaddeaddead;
  /* Metadata */
  uint64_t producer_id = 0;


  PhysicalReg() {}
  PhysicalReg(RegId regid_) {
    classValue = 1;
    virt_reg_idx = id2i(regid_);
    regid = regid_;
  }

  void doRename(uint64_t phyidx){
    assert(!isRenammed);
    isRenammed = true;
    phys_reg_idx = phyidx;
  }
  bool doRenameIfMatchVreg(PhysicalReg &reg, uint64_t phyidx){
    if (!classValue){
      return false;
    }
    if (reg.virt_reg_idx == virt_reg_idx){
      doRename(phyidx);
      return true;
    }
    return false;
  }
  bool operator==(const PhysicalReg& rhs) const {
    return isRenammed == rhs.isRenammed &&
           virt_reg_idx == rhs.virt_reg_idx &&
           phys_reg_idx == rhs.phys_reg_idx;
  }

  std::ostream& str(std::ostream &os) const {
    std::ios init(NULL);
    init.copyfmt(os);
    os << (is_reg_dead ? '*' : ' ');
    os << registerName(virt_reg_idx);
    if (isRenammed){
      os << "\033[38;5;" << (phys_reg_idx * 97) % 256 << 'm';
      os << ":%" << std::setfill('%') << std::setw(4) << phys_reg_idx;
      os << "\x1B[0m";
    }
    os.copyfmt(init);
    return os;
  }

  std::ostream& dumpWithValue(std::ostream &os) const {
    std::ios init(NULL);
    init.copyfmt(os);
    str(os);
    os << ':';
    if (valid){
        os << std::setfill('0') << std::right
           << std::hex << std::setw(16) << value;
    } else {
        os << "uuuuuuuuuuuuuuuu";
    }
    os.copyfmt(init);
    return os;
  }

  void set(uint64_t value_){
    value = value_;
    valid = true;
  }
};

inline std::ostream &operator <<(std::ostream &os,
  const PhysicalReg &reg) {
    return reg.str(os);
}

template <class T>
class PhysicalRegFile
{
  class PhysicalRegHash_t
  {
  public:
    size_t operator()(const PhysicalReg &p) const {
      return p.isRenammed ? p.phys_reg_idx : -p.virt_reg_idx;
    }
  };

  private:
  using arr_t = std::vector<T>;
  arr_t array;
  public:
  using iterator = typename arr_t::iterator;
  using const_iterator = typename arr_t::const_iterator;

  // std::unordered_map<PhysicalReg, T, PhysicalRegHash_t> map;
  public:
  PhysicalRegFile() {}

  T& operator[](PhysicalReg reg){
    fatal_if(!reg.isRenammed, "Reg is not Physical : %s\n", reg);
    fatal_if(!reg.classValue, "Reg is not valid : %s\n", reg);
    // Fix size;
    if (array.size() <= reg.phys_reg_idx){
      array.resize(reg.phys_reg_idx + 1);
    }
    return array[reg.phys_reg_idx];
  }

  /* Direst access from preg index */
  T& operator[](uint64_t preg){
    // Fix size;
    if (array.size() <= preg){
      array.resize(preg + 1);
    }
    return array[preg];
  }

  iterator begin() { return array.begin(); }
  const_iterator begin() const { return array.begin(); }
  const_iterator cbegin() const { return array.cbegin(); }
  iterator end() { return array.end(); }
  const_iterator end() const { return array.end(); }
  const_iterator cend() const { return array.cend(); }

  void setall(T val){
    for (auto& x: array){
      x = val;
    }
  }
};

template <class T>
class ArchRegFile
{
  private:
  using arr_t = std::vector<T>;
  arr_t array;

  public:
  using iterator = typename arr_t::iterator;
  using const_iterator = typename arr_t::const_iterator;

  ArchRegFile() {}
  T& operator[](PhysicalReg reg){
    size_t idx = reg.virt_reg_idx;
    assert(reg.classValue);
    // Fix size;
    if (array.size() <= idx){
      array.resize(idx + 1);
    }
    return array[idx];
  }

  iterator begin() { return array.begin(); }
  const_iterator begin() const { return array.begin(); }
  const_iterator cbegin() const { return array.cbegin(); }
  iterator end() { return array.end(); }
  const_iterator end() const { return array.end(); }
  const_iterator cend() const { return array.cend(); }
  void setall(T val){
    for (auto& x: array){
      x = val;
    }
  }
  bool isall(T val){
    for (auto& x: array){
      if (x != val){
        return false;
      }
    }
    return true;
  }

  bool swap_value(T val, T newval){
    for (int i = 0; i < array.size(); i++){
      if (array[i] == val){
        array[i] = newval;
        return true;
      }
    }
    return false;
  }

  std::string dump_match(T val){
    std::stringstream ss;
    ss << "{ ";
    assert(array.size() <= NB_I2ID);
    for (int i = 0; i < array.size(); i++){
      if (array[i] == val){
        ss << registerName(i) << ' ';
      }
    }
    ss << '}';
    return ss.str();
  }
};



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

    /* Annotate extra uOp inserted on flight */
    uint64_t uop_extra = 0;

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
      if (id.fetchSeqNum == inst_->id.fetchSeqNum){
        return id.uop_extra >= inst_->id.uop_extra;
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
    bool isHighConf = false;
    bool predFromBim = false;

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
    bool needSerialise = false;

    /************ Issue stage ************/
    uint64_t issue_start_ts = 0;
    uint64_t issue_ts = 0;
    bool issue_completed = false;
    /** Source registers values */

    /** memory request generated when load/store */
    DTLBRequestPtr dreq = nullptr;
    Cva6DynInstPtr break_memory_order = bubble();
    uint64_t last_store_id = 0; /* Used to detech mem hazard */

    /************ Execute stage ************/
    bool execute_completed = false;
    bool ex_request_squash = false;
    /** Destination registers values */
    std::vector<PhysicalReg> regs_dst_phy;
    std::vector<PhysicalReg> regs_src_phy;
    uint64_t bb_idx;
    uint64_t delta; // DELME LATER, annotate each register
    std::vector<PhysicalReg> phys_reg_to_free;

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

    /* Scheduler data */
    bool free_reg_at_commit = false;

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

      for (auto &reg: regs_src_phy){
        reg.valid = false;
      }

      for (auto &reg: regs_dst_phy){
        reg.valid = false;
      }
      untrackDreq();
      // Reset Execute
      execute_completed = false;
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

    bool isASquash(){
      assert(execute_completed);
      bool is_serialise = !isFault() &&
          isLastOpInInst() &&
          (staticInst->isSerializeAfter() ||
            staticInst->isSquashAfter());

      bool is_addr_unmatch = triedToPredict &&
                            *predictedTarget != *pc_next;
      bool is_fault = isFault();

      bool need_squash = is_addr_unmatch ||
                          is_fault ||
                          is_serialise;
      return need_squash;
    }

    bool isTaken(){
      assert(!isFault());
      assert(staticInst->isCondCtrl());
      assert(execute_completed);
      std::unique_ptr<PCStateBase> noTakenPc;
      set(noTakenPc, pc);
      staticInst->advancePC(*noTakenPc);
      return *noTakenPc != *pc_next;
    }

    void setFaultFrontend(Fault fault_){
      fault = fault_;
    }

    void setFaultEx(Fault fault_){
      fault_ex = fault_;
    }

    // void switchToReplayFault(){
    //   assert(!isFault());
    //   fault_ex = std::make_shared(FlushBeforeFault(pc->instAddr()));
    // }

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
    template<class Src, class Dst>
    using cv_t = std::conditional_t<std::is_const<Src>{}, Dst const, Dst>;

    template<class T>
    static cv_t<T, PhysicalReg>& id2physical(RegId& regid, T& vec) {
      for (auto &r: vec){
        if (r.regid == regid){
          return r;
        }
      }
      fatal("Unrecheable: reg must exist\n");
    }

    RegVal getSrcRegOperand(int idx) const {
      assert(staticInst);
      assert(idx < staticInst->numSrcRegs());
      RegId regid = staticInst->srcRegIdx(idx);
      if (regid.classValue() == InvalidRegClass){
        return 0;
      }
      const PhysicalReg &reg = id2physical(regid, regs_src_phy);
      assert(reg.valid);
      return reg.value;
    }

    /* For compatibility */
    RegVal getDstRegOperand(int idx) const {
      assert(staticInst);
      assert(idx < staticInst->numDestRegs());
      RegId regid = staticInst->destRegIdx(idx);
      if (regid.classValue() == InvalidRegClass){
        return 0;
      }
      const PhysicalReg &reg = id2physical(regid, regs_dst_phy);
      assert(reg.valid);
      return reg.value;
    }

    void setDstRegOperand(int idx, RegVal val){
      assert(staticInst);
      assert(idx < staticInst->numDestRegs());
      RegId regid = staticInst->destRegIdx(idx);
      if (regid.classValue() == InvalidRegClass){
        return; // Nothing to set
      }
      PhysicalReg &reg = id2physical(regid, regs_dst_phy);
      reg.valid = true;
      reg.value = val;
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
