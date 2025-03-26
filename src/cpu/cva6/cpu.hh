/**
 * @file
 *
 *  Top level definition of the Cva6 in-order CPU model
 */

#pragma once

#include "base/compiler.hh"
#include "base/random.hh"
#include "cpu/base.hh"
#include "cpu/cva6/stats.hh"
#include "cpu/simple_thread.hh"
#include "debug/Cva6X.hh"
#include "mem/cache/cache.hh"
#include "mem/packet.hh"
#include "params/BaseCva6CPU.hh"
#include "sim/faults.hh"

namespace gem5 {
namespace cva6 {


/** Forward declared to break the cyclic inclusion dependencies between
 *  pipeline and cpu */
class Pipeline;

/** Cva6 will use the SimpleThread state for now */
typedef SimpleThread Cva6Thread;

} // namespace cva6


/**
 *  Cva6CPU is an in-order CPU model with 5 pipeline stages:
 *
 *  Fetch1  - fetches lines from memory
 *  Fetch2  - decomposes lines into macro-op instructions
 *  Decode  - decomposes macro-ops into micro-ops
 *  Issue   - Issues instrutions to fus
 *  Execute - executes those micro-ops
 *
 *  This pipeline is carried in the Cva6CPU::pipeline object.
 *  The exec_context interface is not carried by Cva6CPU but by
 *      the instructions
 */
class Cva6CPU : public BaseCPU
{
  public:
    /** pipeline is a container for the clockable pipeline stage objects.
     *  Elements of pipeline call TheISA to implement the model. */
    cva6::Pipeline *pipeline;

  public:
    /** These are thread state-representing objects for this CPU. */
    cva6::Cva6Thread *thread;
     /** Provide a non-protected base class for Cva6's Ports as derived
     *  classes are created by Fetch1 and Execute */
    class Cva6CPUPort : public RequestPort
    {
      public:
        /** The enclosing cpu */
        Cva6CPU &cpu;

      public:
        Cva6CPUPort(const std::string& name_, Cva6CPU &cpu_)
            : RequestPort(name_), cpu(cpu_)
        { }
    };

    /** Exposable data port */
    class DcachePort : public Cva6CPUPort
    {
      public:
        DcachePort(std::string name, Cva6CPU &cpu) :
            Cva6CPUPort(name, cpu)
        { }
      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override { assert(0); }
        bool isSnooping() const override { return true; }
        void recvTimingSnoopReq(PacketPtr pkt);
        void recvFunctionalSnoop(PacketPtr pkt) override { }
    };

    DcachePort dcachePort;
    Cache *dcache;
    Cache *icache;


  public:
     /** Return a reference to the data port. */
    RequestPort &getDataPort() override { return dcachePort; }

    /** Return a reference to the instruction port. */
    RequestPort &getInstPort() override;

  public:
    Cva6CPU(const BaseCva6CPUParams &params);

    ~Cva6CPU();

  public:
      /** Starting, waking and initialisation */
    void init() override;
    void startup() override;
    void wakeup(ThreadID tid) override;

    /** Processor-specific statistics */
    cva6::Cva6Stats stats;

    /** Stats interface from SimObject (by way of BaseCPU) */
    void regStats() override;

    /** Simple inst count interface from BaseCPU */
    Counter totalInsts() const override;
    Counter totalOps() const override;

    void serializeThread(CheckpointOut &cp, ThreadID tid) const override;
    void unserializeThread(CheckpointIn &cp, ThreadID tid) override;

    /** Serialize pipeline data */
    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;

    /** Drain interface */
    DrainState drain() override;
    void drainResume() override;
    /** Signal from Pipeline that Cva6CPU should signal that a drain
     *  is complete and set its drainState */
    void signalDrainDone();
    void memWriteback() override;

    /** Switching interface from BaseCPU */
    void switchOut() override;
    void takeOverFrom(BaseCPU *old_cpu) override;

    /** Thread activation interface from BaseCPU. */
    void activateContext(ThreadID thread_id) override;
    void suspendContext(ThreadID thread_id) override;

    /** The tick method in the Cva6CPU is simply updating the cycle
     * counters as the ticking of the pipeline stages is already
     * handled by the Pipeline object.
     */
    void tick() { updateCycleCounters(BaseCPU::CPU_STATE_ON); }

    /** Interface for stages to signal that they have become active after
     *  a callback or eventq event where the pipeline itself may have
     *  already been idled. */
    EventFunctionWrapper *fetchEventWrapper;


    ThreadContext *getContext() {
      return BaseCPU::getContext((ThreadID)0);
    }
    void wakeup() {
      Cva6CPU::wakeup((ThreadID)0);
    }
    AddressMonitor* getCpuAddrMonitor() {
      return BaseCPU::getCpuAddrMonitor((ThreadID)0);
    }
    BaseInterrupts* getInterruptController() {
      return BaseCPU::getInterruptController((ThreadID)0);
    }
    bool checkInterrupts() {
      return BaseCPU::checkInterrupts((ThreadID)0);
    }

    // void finishTranslation(const Fault &fault,
    // const RequestPtr &mem_req) { }

};

/** SpeculativeFault:
 * this fault appends when memory access are
 * invalids. Sometimes a memory access will be speculatively
 * executed along a branch that will end up not being taken where the
 * address is invalid.  In that case, return a fault rather than trying
 * to execute it (which will cause a panic).
 *
 * Even if RISC-V allows unaligned memory accesses, we catch them with
 * this fault.
*/
class SpeculativeFault : public FaultBase
{
  private:
    std::string panicStr;
  public:
    SpeculativeFault(std::string _str) : panicStr(_str) {}

    FaultName
    name() const override
    {
        return "Speculative fault";
    }
    void invoke(ThreadContext *tc, const StaticInstPtr &inst =
                nullStaticInstPtr) override {
        fatal("%s: %s must be never executed\n", name(),
            panicStr.c_str());
    }
};

class NeverCommitFault : public FaultBase
{
  private:
  public:
  /* Singleton */
  NeverCommitFault() {}
  // void operator=(NeverCommitFault const&)   = delete;
  // NeverCommitFault(NeverCommitFault const&) = delete;

  static std::shared_ptr<FaultBase> fault(){
    static auto s = std::make_shared<NeverCommitFault>();
    return s;
  }
  FaultName name() const override { return "Never Commit Fault"; }
  void invoke(ThreadContext *tc, const StaticInstPtr &inst =
              nullStaticInstPtr) override {
    fatal("%s: must be never executed\n", name());
  }
};

class FlushBeforeFault : public FaultBase
{
  private:
  uint64_t pc;
  public:
  FlushBeforeFault(uint64_t pc_) : pc(pc_) {}
  FaultName name() const override { return "Flush Before Fault"; }
  void invoke(ThreadContext *tc, const StaticInstPtr &inst =
              nullStaticInstPtr) override {
    tc->pcState(pc);
  }
};

/** Structure to hold SenderState info through
 *  translation and memory accesses. */
class DTLBRequest :
    public BaseMMU::Translation, /* For TLB lookups */
    public Packet::SenderState /* For packing into a Packet */
{
  public:
  static const int BASESIZE = 16;

  protected:
    /** Owning cpu */
    Cva6CPU &cpu;
  public:
    // The public interface !
    Fault fault = NoFault; // All faults
    RequestPtr req; // Used in pipeline
    PacketPtr pkt = nullptr; // Used for commitAcc
    // Request::Flags flags;
    bool is_cl_req_inorder = true;

  protected:
    // Requested data
    uint8_t *data;
    uint64_t *res;
    BaseMMU::Mode mode;

    // Effective packed
    uint8_t *raw_data;
    uint64_t raw_size;
    RequestPtr req2;
    PacketPtr pkt2 = nullptr;

  public:
    // Forward data used to emmit mem request before store completion
    uint64_t fwval = 0;
    uint64_t fwmask = 0; /* 0b1 means bit is forwarded */

    enum DTLBRequestState
    {
      NotIssued = 0, /* Just been made */
      InTranslation, /* Issued to ITLB, must wait for reqply */
      Translated, /* Translation complete */
      InMemory, /* Issued to memory, must wait for response */
      Complete, /* Complete.  Either a fault, or a fetched line */
      End
    };
    class Fsm
    {
      private:
      Cva6CPU &cpu;
      DTLBRequest *req; /* Englobing req*/
      static inline const std::string _names[] = {
        "____NotIssued",
        "InTranslation",
        "___Translated",
        "_____InMemory",
        "_____Complete"
      };
      uint64_t last_time = 0;
      uint64_t delays[5] = { 0 };
      DTLBRequestState state = NotIssued;
      uint64_t time(){
        return cpu.curCycle();
      }

      class SingletonStats
      {
        private:
        struct Stats : public statistics::Group
        {
          statistics::Distribution inMemoryLatencies;
          statistics::Scalar *delays[5];
          Stats(const std::string &name, BaseCPU &cpu) :
            statistics::Group(&cpu, name.c_str()),
            ADD_STAT(inMemoryLatencies, "inMemoryLatencies") {
            for (int i = 0; i < 5; i++){
              delays[i] = new statistics::Scalar(this, _names[i].c_str());
            }
            inMemoryLatencies
              .init(0,100,1)
              .flags(statistics::pdf);
          };
        } stats;
        public:
        SingletonStats(const std::string &name, BaseCPU &cpu) :
          stats(name, cpu) {}
        void sample(uint64_t delta, DTLBRequestState x){
          *stats.delays[(int)x] += delta;
          if (x == InMemory){
            stats.inMemoryLatencies.sample(delta);
          }
        }
      };

      static inline SingletonStats *ss = nullptr;
      SingletonStats *getSS(){
        if (ss == nullptr){
          printf("New SING!!\n");
          ss = new SingletonStats(cpu.name() + ".dreqfsm", cpu);
        }
        return ss;
      }

      public:
      Fsm(Cva6CPU &cpu_, DTLBRequest *req_) : cpu(cpu_), req(req_) {
        last_time = time();
      }
      Fsm& operator=(DTLBRequestState x) {
        uint64_t now = time();
        uint64_t delta = now - last_time;
        last_time = now;
        delays[(int)state] += delta;
        if (req->mode == BaseMMU::Read){
          getSS()->sample(delta, state);
        }
        state = x; /* Set new state */
        return *this;
      }
      bool operator==(DTLBRequestState rhs) {
        return state == rhs;
      }
      bool operator!=(DTLBRequestState rhs) {
        return state != rhs;
      }
      const std::string& str(){
        assert((int)state < (int)DTLBRequestState::End);
        return _names[(int)state];
      }
      uint64_t inMemoryDelay(){
        return delays[InMemory];
      }

    };
    Fsm state;
    bool inuse = true;

    void setupfault();

  public:
    DTLBRequest(
      Cva6CPU &cpu_,
      Addr pc, /* For prefetchers ? */
      Addr addr, /* The virtual address */
      unsigned int size, /* The request size */
      Request::Flags flags_,
      const std::vector<bool>& be, /* For masker writes only */
      uint8_t *data_, // Data for stores
      uint64_t *res_, // For Store
      AtomicOpFunctorPtr amo_op, // For AMO
      BaseMMU::Mode mode_) :
      SenderState(),
      cpu(cpu_),
      res(res_),
      mode(mode_),
      state(cpu_, this)
    {
      // Check unalignement
      raw_size = BASESIZE; // cpu.cacheLineSize();
      uint64_t addr_mask_offset = (raw_size - 1);

      Addr split_addr = roundDown(addr + size - 1, raw_size);
      assert(split_addr <= addr || split_addr - addr < raw_size);
      assert(std::all_of(be.begin(), be.end(), [](bool v) { return v; }));
      assert(size <= raw_size);

      req = std::make_shared<Request>(
          addr, size, flags_, cpu.dataRequestorId(),
          pc, cpu.thread->contextId(), std::move(amo_op));
      req->taskId(cpu.taskId());
      assert(req->hasPC());

      /* If req have unaligned addr, setup a fault */
      setupfault();
      // if (req->isCondSwap()){
      //   assert(res);
      //   req->setExtraData(*res);
      // }

      // if (be){
      //   assert(be.size() == size);
      //   req->setByteEnable(*be);
      // }
      raw_data = new uint8_t[raw_size]; // Data line
      data = raw_data + (addr & addr_mask_offset); // Data in line

      if (fault == NoFault){
        if (mode == BaseMMU::Read){
          // uint64_t rc = 0xdeadbeefdeadbeef; // Payload as a sanity check,
          memset (raw_data, 0xaa, raw_size);
        } else {
          if (data_ == NULL) {
              memset(data, 0, size); // cache block cleaning request
          } else {
              memcpy(data, data_, size);
          }
        }
      }
    }

    /** Launch request in system */
    void launch(){ translateTiming(); }

    bool isLaunched() { return state != DTLBRequestState::NotIssued; }
    bool isInTranslation() { return state == DTLBRequestState::InTranslation; }
    bool isTranslated() { return state == DTLBRequestState::Translated; }
    bool isInMemory() { return state == DTLBRequestState::InMemory; }
    bool isCompleted() { return state == DTLBRequestState::Complete; }

    bool isOutsideCpu() {
        return state == DTLBRequestState::InTranslation ||
               state == DTLBRequestState::InMemory;
    }

    void translateTiming();
    void translateFunctional();

    /** BaseMMU::Translation interface */

  protected:
    /** Interface for DTLB responses.  We can handle delay, so don't
     *  do anything */
    void markDelayed() { }

    /** Interface for DTLB responses.  Populates self and then passes
     *  the request on to the ports' handleTLBResponse member
     *  function */
    void finish(const Fault &fault_, const RequestPtr &request_,
                ThreadContext *tc, BaseMMU::Mode mode) override;
  public:
    void sendData();

  public: // Callback
    void onRecv(PacketPtr pkt);

  public:
    /** For DPRINTF usage */
    std::string name();

    void untrack(){
      if (isOutsideCpu()){
        inuse = false; // Mark as untracked : delayed free
      } else {
        delete this;
      }
    }

    ~DTLBRequest() {
      // req = NULL;
      if (pkt2){
        delete pkt2;
      }
      if (pkt){
        delete pkt;
      }
      delete [] raw_data; // test
    }

  public:
    /* Setter that bypass everything */
    void complete_forward(uint64_t val);

    /* Default getter */
    uint64_t getPaddr() { return req->getPaddr(); }
    uint16_t getSize() { return req->getSize(); }
    RegVal getData();
    void setRawData(uint64_t val) { memcpy(data, &val, req->getSize()); }

    /* Cacheline getter */
    bool isCl(){ return pkt2 != nullptr; }
    uint8_t* getClData(){
      assert(isCl());
      return raw_data;
    }
    uint64_t getClPaddr(){ return req->getPaddr() & ~(raw_size - 1); }
    uint64_t getClVaddr(){ return req->getVaddr() & ~(raw_size - 1); }
    uint16_t getClSize(){ return raw_size; }

    /* Double Word getter */
    /* xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx */
    /* @DW - @CL = offset in line ? */
    uint64_t getDWData(){
      /*assert(isCl()); if not cl: invalid data */
      return *(uint64_t*)(raw_data + (getDWPaddr() - getClPaddr()));
    }
    uint64_t getDWPaddr(){ return req->getPaddr() & ~0b111; }
    uint64_t getDWVaddr(){ return req->getVaddr() & ~0b111; }
    uint16_t getDWSize(){ return 8; }

    bool is_prefetch_mode = false;
    bool prefetch_mode_failed = false;
    void setPrefetchMode(){
      assert(state == Translated);
      is_prefetch_mode = true;
    }
    bool isBufferable(){
      // assert(req->hasPaddr()); // No fault VP HIT !
      return !req->isUncacheable() &&
            !req->isAtomic() &&
            !req->isLLSC() &&
            !req->isSwap()&&
            !req->isCacheInvalidate() &&
            !req->isCacheClean() &&
            !req->isLockedRMW();
    }
    uint64_t inMemoryDelay(){
      return state.inMemoryDelay();
    }

};

typedef DTLBRequest* DTLBRequestPtr;

} // namespace gem5
