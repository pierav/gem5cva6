/**
 * @file
 *
 *  Contains class definitions for data flowing between pipeline stages in
 *  the top-level structure portion of this model.
 */

#pragma once

#include "cpu/base.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "debug/Cva6FU.hh"

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

    /* Squash to a specific pc */
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
    /* Squash to the latest valid pc */
    static BranchData
    SquashAt(Cva6CPU &cpu){
      return BranchData::SquashAt(cpu.getContext()->pcState());
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
    std::ostream& dump(std::ostream &os) const;
};


/** Line fetch data in the forward direction.  Contains a single cache line
 *  (or fragment of a line), its address, a sequence number assigned when
 *  that line was fetched  */
class ForwardLineData
{
  public:
    Addr lineBaseAddr = 0; /** First byte address in the line. */
    std::unique_ptr<PCStateBase> pc; /** PC of the first */
    Addr fetchAddr; /** Address of this line of data */
    unsigned int lineWidth = 0; /** Explicit line width  */
    Fault fault = NoFault;     /** This line has a fault. */
    uint8_t *line = nullptr;   /** Line data. */
    Packet *packet = nullptr;  /** Packet from which the line is taken */

  public:
    ForwardLineData() {}

    ~ForwardLineData() { line = nullptr; }

  public:
    /** This is a fault, not a line */
    bool isFault() const { return fault != NoFault; }

    /** Set fault and possible clear the bubble flag */
    void setFault(Fault fault_) { fault = fault_; }

    /** Use the data from a packet as line instead of allocating new
     *  space.  On destruction of this object, the packet will be destroyed */
    void adoptPacketData(Packet *packet) {
      this->packet = packet;
      lineWidth = packet->req->getSize();
      line = packet->getPtr<uint8_t>();
    }

    /** Free this ForwardLineData line.  */
    void freeLine(){
      /* Only free lines in non-faulting, non-bubble lines */
      if (!isFault()) {
        assert(line);
        /* If packet is not NULL then the line must belong to the packet so
        *  we don't need to separately deallocate the line */
        if (packet) {
            delete packet;
        } else {
            delete [] line;
        }
        line = nullptr;
      }
    }

    std::ostream& dump(std::ostream &os) const;
};

class ForwardLineDataReg
{
  private:
  /* A little hacked allocator */
  ForwardLineData *buff;
  uint64_t idx;
  public:
  ForwardLineData* alloc(){ return &buff[(idx++)%maxsize]; }

  private:
  size_t maxsize;
  public:
  ForwardLineDataReg(size_t size=4) :maxsize(size) {
    buff = new ForwardLineData[maxsize];
  }
  std::deque<ForwardLineData*> fifo;
  public:
  bool canPush() { return fifo.size() < 4; }
  void push(ForwardLineData *line) { fifo.push_back(line); }
  bool canPop() { return fifo.size(); }
  ForwardLineData *pop() {
    ForwardLineData *ret = fifo.front();
    fifo.pop_front();
    return ret;
  }
  ForwardLineData *front(){ return fifo.front(); }
  void flush() { fifo.clear(); }
  size_t size() { return fifo.size(); }
  std::ostream& dump(std::ostream &os) const;
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
    virtual size_t size() = 0;
};

/** Forward sbe between instructions stages. */
class ForwardInstData :
  public ForwardInstDataPushIntf,
  public ForwardInstDataPopIntf
{
  protected:
    /** Instructions fifo */
    std::deque<Cva6DynInstPtr> insts;
    size_t maxsize;

  public:
    explicit ForwardInstData(size_t size_=-1) : maxsize(size_) { ; }

    /** Push interface */
    bool empty() { return insts.empty(); }
    bool canPush() { return insts.size() < maxsize; }
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

    size_t size() { return insts.size(); }
    /** Reporting */
    std::ostream &dump (std::ostream &os) const;
};

bool maskMatchVaddrInst(Cva6DynInstPtr i1, Cva6DynInstPtr i2, uint64_t mask);

class MatchAddrIntf
{
  public:
  virtual bool isMaskMatchVaddr(Cva6DynInstPtr inst, uint64_t mask) = 0;
  bool isPageOffsetMatches(Cva6DynInstPtr inst){
      return isMaskMatchVaddr(inst, 0b111111111000);
  }
  bool isClMatch(Cva6DynInstPtr inst, uint64_t clsize){
      uint64_t mask = ((1 << 12) - 1); // 0b111111111111;
      mask &= ~(clsize - 1); // 0b111111110000
      // assert(mask == 0b111111110000);
      return isMaskMatchVaddr(inst, mask);
  }
};

class Cva6DynInstChunk : public Named, public MatchAddrIntf
{
  using ContainerT = std::deque<Cva6DynInstPtr>;

  protected:
    ContainerT chunk;
    size_t sizemax;

  public:
    Cva6DynInstChunk(const std::string &name, size_t size_=-1) :
      Named(name), sizemax(size_){ }

    bool canPush(){
      return chunk.size() < sizemax;
    }

    void push(Cva6DynInstPtr inst){
      chunk.push_back(inst);
    }

    bool canPop(Cva6DynInstPtr inst){
      return std::find(chunk.begin(), chunk.end(), inst) != chunk.end();
    }

    void pop(Cva6DynInstPtr inst){
      chunk.erase(std::find(chunk.begin(), chunk.end(), inst));
    }

    void erase(Cva6DynInstPtr inst){
      chunk.erase(std::find(chunk.begin(), chunk.end(), inst));
    }

    Cva6DynInstPtr pop(){
      Cva6DynInstPtr ret = chunk.front();
      chunk.pop_front();
      return ret;
    }
    ContainerT::iterator erase(ContainerT::iterator it){
      return chunk.erase(it);
    }

    // InO implem
    // void flushfrom(Cva6DynInstPtr _inst) {
    //   while (!chunk.empty() &&
    //     chunk.back()->isAfterOrEqual(_inst)){
    //     DPRINTF(Cva6FU, "Flush %s\n", *chunk.back());
    //     chunk.pop_back();
    //   }
    // }

    // OoO implem
    void flushfrom(Cva6DynInstPtr _inst) {
      auto it = chunk.begin();
      while (it != chunk.end()){
        if ((*it)->isAfterOrEqual(_inst) && !(*it)->commit_completed){
          DPRINTF(Cva6FU, "Flush %s\n", **it);
          it = chunk.erase(it);// erase and go to next
        } else{
          ++it;  // go to next
        }
      }
    }

    void flush(){ flushfrom(Cva6DynInst::bubble()); }

    bool empty()                 { return chunk.empty(); }
    size_t size()                { return chunk.size(); }
    Cva6DynInstPtr front()       { return chunk.front(); }
    Cva6DynInstPtr back()        { return chunk.back(); }
    ContainerT::iterator begin() { return chunk.begin(); }
    ContainerT::iterator end()   { return chunk.end(); }

    Cva6DynInstPtr& operator[](int idx)      { return chunk[idx]; }
    Cva6DynInstPtr operator[](int idx) const { return chunk[idx]; }

    bool isMaskMatchVaddr(Cva6DynInstPtr inst, uint64_t mask) override {
      assert(inst->dreq); // Inst must be issued
      Addr addr_masked = inst->dreq->req->getVaddr() & mask;
      // Check if the page offset matches
      for (Cva6DynInstPtr i2: chunk){
        if (i2 == inst){
          return false; // TODO care OoO
        }
        if ((i2->dreq->req->getVaddr() & mask) == addr_masked){
          return true;
        }
      }
      fatal("Unrecheable\n");
      return false;
    }
};


inline std::ostream &
operator <<(std::ostream &os, const BranchData &x){
  return x.dump(os);
}

inline std::ostream &
operator <<(std::ostream &os, const ForwardInstData &x){
  return x.dump(os);
}

inline std::ostream &
operator <<(std::ostream &os, const ForwardLineData &x){
  return x.dump(os);
}

inline std::ostream &
operator <<(std::ostream &os, const ForwardLineDataReg &x){
  return x.dump(os);
}

} // namespace cva6
} // namespace gem5

