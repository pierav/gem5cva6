/**
 * @file
 *
 *  Contains class definitions for data flowing between pipeline stages in
 *  the top-level structure portion of this model.  Latch types are also
 *  defined which pair forward/backward flowing data specific to each stage
 *  pair.
 */

#pragma once

#include "cpu/base.hh"
#include "cpu/cva6/buffers.hh"
#include "cpu/cva6/dyn_inst.hh"

namespace gem5 {
namespace cva6 {

/** Forward data betwen Execute and others stages */
class BranchData
{
  public:
    /* Squash informations */
    bool need_squash = false;
    std::unique_ptr<PCStateBase> squash_target;

    /* Bpred informations */
    InstSeqNum num = 0;
    bool is_predicted = false;
    std::unique_ptr<PCStateBase> target;
    bool actually_taken = false;

    /** Bubble generation */
  private:
    static BranchData *bloup;
  public:
    static BranchData& bubble() { return *bloup; }

  public:
    BranchData() {}

    BranchData(
      bool is_predicted_,
      bool need_squash_,
      InstSeqNum num_,
      const PCStateBase &target_,
      bool actually_taken_) :
      need_squash(need_squash_),
      num(num_),
      is_predicted(is_predicted_),
      actually_taken(actually_taken_)
    {
        set(target, target_);
        set(squash_target, target_);
    }

    BranchData(const BranchData &other) :
        need_squash(other.need_squash),
        num(other.num),
        is_predicted(other.is_predicted),
        actually_taken(other.actually_taken)
    {
        set(target, other.target);
        set(squash_target, other.squash_target);
    }

    void setSquashTarget(const PCStateBase &target_){
      set(squash_target, target_);
    }
    static BranchData
    SquashAt(const PCStateBase &target){
      return BranchData(
        false, /* Is predicted : need update */
        true, /* Need squash */
        0, /* sn:0 Squash everything */
        target,
        true // Unused
      );
    }
    BranchData &
    operator=(const BranchData &other)
    {
        is_predicted = other.is_predicted;
        need_squash = other.need_squash;
        num = other.num;
        actually_taken = other.actually_taken;
        set(target, other.target);
        set(squash_target, other.squash_target);
        return *this;
    }

    bool isBubble() const { return !(is_predicted || need_squash); }

    /** As static isStreamChange but on this branch data */
    bool isStreamChange() const { return need_squash; }

    std::string dump() const {
        std::ostringstream os;
        os << "BranchData(";
        if (isBubble()) {
          os << "bubble";
        } else {
          if (is_predicted){
            os << "prediction ";
            if (need_squash){
              os << "KO";
            } else {
              os << "OK";
            }
          }
          os << ";num=" << num;
          os << ";0x" << std::hex << target->instAddr() << std::dec;
          os << ';';
          if (need_squash){
            os << " [squash:0x" << std::hex
               << squash_target->instAddr() << std::dec
               << "]";
          }
        }
        os << ")";
        return os.str();
    }
};

/** Print BranchData contents in a format suitable for DPRINTF comments */
std::ostream &operator <<(std::ostream &os, const BranchData &branch);

/** Line fetch data in the forward direction.  Contains a single cache line
 *  (or fragment of a line), its address, a sequence number assigned when
 *  that line was fetched and a bubbleFlag that can allow ForwardLineData to
 *  be used to represent the absence of line data in a pipeline. */
class ForwardLineData /* : public ReportIF, public BubbleIF */
{
  private:
    /** This line is a bubble.  No other data member is required to be valid
     *  if this is true
     *  Make lines bubbles by default */
    bool bubbleFlag = true;

  public:
    /** First byte address in the line.  This is allowed to be
     *  <= pc.instAddr() */
    Addr lineBaseAddr = 0;

    /** PC of the first inst within this sequence */
    std::unique_ptr<PCStateBase> pc;

    /** Address of this line of data */
    Addr fetchAddr;

    /** Explicit line width, don't rely on data.size */
    unsigned int lineWidth = 0;

  public:
    /** This line has a fault.  The bubble flag will be false and seqNums
     *  will be valid but no data will */
    Fault fault = NoFault;

    /** Line data.  line[0] is the byte at address pc.instAddr().  Data is
     *  only valid upto lineWidth - 1. */
    uint8_t *line = nullptr;

    /** Packet from which the line is taken */
    Packet *packet = nullptr;

  public:
    ForwardLineData() {}
    ForwardLineData(const ForwardLineData &other) :
        bubbleFlag(other.bubbleFlag), lineBaseAddr(other.lineBaseAddr),
        pc(other.pc->clone()), fetchAddr(other.fetchAddr),
        lineWidth(other.lineWidth), fault(other.fault),
        line(other.line), packet(other.packet)
    {}
    ForwardLineData &
    operator=(const ForwardLineData &other)
    {
        bubbleFlag = other.bubbleFlag;
        lineBaseAddr = other.lineBaseAddr;
        set(pc, other.pc);
        fetchAddr = other.fetchAddr;
        lineWidth = other.lineWidth;
        fault = other.fault;
        line = other.line;
        packet = other.packet;
        return *this;
    }

    ~ForwardLineData() { line = NULL; }

  public:
    /** This is a fault, not a line */
    bool isFault() const { return fault != NoFault; }

    /** Set fault and possible clear the bubble flag */
    void setFault(Fault fault_);

    /** In-place initialise a ForwardLineData, freeing and overridding the
     *  line */
    void allocateLine(unsigned int width_);

    /** Use the data from a packet as line instead of allocating new
     *  space.  On destruction of this object, the packet will be destroyed */
    void adoptPacketData(Packet *packet);

    /** Free this ForwardLineData line.  Note that these are shared between
     *  line objects and so you must be careful when deallocating them.
     *  Copying of ForwardLineData can, therefore, be done by default copy
     *  constructors/assignment */
    void freeLine();

    /** BubbleIF interface */
    static ForwardLineData bubble() { return ForwardLineData(); }
    bool isBubble() const { return bubbleFlag; }

    /** ReportIF interface */
    void reportData(std::ostream &os) const;
};


/** ForwardInstData Producer interface */
class ForwardInstDataPushIntf
{
  public:
    virtual bool empty() = 0;
    virtual bool canPush() = 0;
    virtual void push(Cva6DynInstPtr inst) = 0;
};

/** ForwardInstData Consumer interface */
class ForwardInstDataPopIntf
{
  public:
    virtual bool canPop() = 0;
    virtual Cva6DynInstPtr front() = 0;
    virtual Cva6DynInstPtr pop() = 0;
    virtual void flush() = 0;
    virtual void flushfrom(Cva6DynInstPtr inst_) = 0;
};

/** Forward sbe between instructions stages. */
class ForwardInstData :
  public ForwardInstDataPushIntf,
  public ForwardInstDataPopIntf
{
  protected:
    /** Instructions fifo */
    std::deque<Cva6DynInstPtr> insts;
    size_t size;

  public:
    explicit ForwardInstData(size_t size_=-1) : size(size_) { ; }

    /** Push interface */
    bool empty() { return insts.empty(); }
    bool canPush() { return insts.size() < size; }
    void push(Cva6DynInstPtr inst) { insts.push_back(inst); }

    /** Pop interface */
    bool canPop() { return !insts.empty(); }
    Cva6DynInstPtr front() { return insts.front(); }
    Cva6DynInstPtr pop() {
      Cva6DynInstPtr ret = insts.front();
      insts.pop_front();
      return ret;
    }
    void flush() { insts.clear(); }
    void flushfrom(Cva6DynInstPtr inst_){
       while (!insts.empty() &&
        insts.back()->isAfterOrEqual(inst_)){
        insts.pop_back();
      }
    }

    /** Reporting */
    void reportData (std::ostream &os) const;
    std::string dump();
};


} // namespace cva6
} // namespace gem5

