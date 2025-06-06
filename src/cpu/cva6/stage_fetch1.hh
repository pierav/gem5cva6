/**
 * @file stage_fetch1.hh
 * @author Pierre Ravenel (pravenel@kalrayinc.com)
 * @brief
 * @version 1.0
 * @date 2023-05-25
 *
 *  Fetch1 is responsible for fetching "lines" from memory and passing
 *  them to Fetch2
 */

#pragma once

#include "arch/generic/mmu.hh"
#include "base/named.hh"
#include "cpu/base.hh"
#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/pipe_data.hh"
#include "mem/packet.hh"

namespace gem5 {
namespace cva6 {


class Fetch1 : public Named
{
  protected:
    /** Exposable fetch port */
    class IcachePort : public Cva6CPU::Cva6CPUPort
    {
      protected:
        /** My owner */
        Fetch1 &fetch;

      public:
        IcachePort(std::string name, Fetch1 &fetch_, Cva6CPU &cpu) :
            Cva6CPU::Cva6CPUPort(name, cpu), fetch(fetch_)
        { }

      protected:
        bool recvTimingResp(PacketPtr pkt) {
          FetchRequestPtr rep =
            safe_cast<FetchRequestPtr>(pkt->popSenderState());
          rep->onRecv(pkt);
          return true;
        }
        void recvReqRetry() { assert(0); }
    };

  public:

    class FetchRequest :
        public BaseMMU::Translation, /* For TLB lookups */
        public Packet::SenderState /* For packing into a Packet */
    {

      protected:
        /** Owning cpu */
        Cva6CPU &cpu;
        /** Owning fetch stage */
        Fetch1& fetch;

      public:
        /** Progress of this request through address translation and
         *  memory */
        enum FetchRequestState
        {
            NotIssued, /* Just been made */
            InTranslation, /* Issued to ITLB, must wait for reqply */
            Translated, /* Translation complete */
            RequestIssuing, /* Issued to memory, must wait for response */
            Complete, /* Complete.  Either a fault, or a fetched line */
            End
        };

        FetchRequestState state;


        /** FetchRequests carry packets */
        PacketPtr packet;

        /** The underlying request that this fetch represents */
        RequestPtr request;

        /** PC to fixup with line address */
        Addr pc;

        /** Fill in a fault if one happens during fetch, check this by
         *  picking apart the response packet */
        Fault fault;

        /* inuse flag used for defered destruction. */
        bool inuse = true;

      public:
        FetchRequest(Cva6CPU &cpu_, Fetch1 &fetch_, Addr pc_) :
            SenderState(),
            cpu(cpu_),
            fetch(fetch_),
            state(NotIssued),
            packet(NULL),
            request(),
            pc(pc_),
            fault(NoFault)
        {
            request = std::make_shared<Request>();
        }

        ~FetchRequest();

        void translateTiming();

        void sendData();
        void onRecv(PacketPtr pkt);

        /** Report interface */
        void reportData(std::ostream &os) const;

        /** Is this line out of date with the current stream/prediction
         *  sequence and can it be discarded without orphaning in flight
         *  TLB lookups/memory accesses? */
        bool isDiscardable() const;

        bool isLaunched() { return state != NotIssued; }
        bool isComplete() { return state == Complete; }
        bool isTranslated() { return state == Translated; }

        bool isOutsideCPU() {
          return state == InTranslation || state == RequestIssuing;
        }

        void untrack() {
          if (isOutsideCPU()){
            inuse = false; // Mark as untracked
          } else {
            delete this;
          }
        }


      public:
        /** For DPRINTF usage */
        std::string name();

      protected:
        /** BaseMMU::Translation interface */

        /** Interface for ITLB responses.  We can handle delay, so don't
         *  do anything */
        void markDelayed() { }

        /** Interface for ITLB responses.  Populates self and then passes
         *  the request on to the ports' handleTLBResponse member
         *  function */
        void finish(const Fault &fault_, const RequestPtr &request_,
                    ThreadContext *tc, BaseMMU::Mode mode);

    };

    typedef FetchRequest *FetchRequestPtr;

  protected:
    /** Pointer back to the containing CPU */
    Cva6CPU &cpu;
    /** Input port carrying branch requests from Execute */
    BranchData &resolved_branch;
    /** Output port carrying read lines to Fetch2 */
    ForwardLineDataReg& out;
    /** Input carrying branch predictions from Fetch2 */
    BranchData &prediction;
    /** IcachePort to pass to the CPU. */
    IcachePort icachePort;
    /** Maximum fetch width in bytes. */
    unsigned int maxLineWidth;

  protected:

    struct Fetch1ThreadInfo
    {
      /** The base PC of streams */
      std::unique_ptr<PCStateBase> pc;
      /** The address we're currently fetching lines from. */
      Addr fetchAddr = 0;
    };

    Fetch1ThreadInfo fetchInfo;
    /** Queue of address translated requests from Fetch1 */
    std::deque<FetchRequestPtr> requests;
    /** Queue of in-memory system requests and responses */
    std::deque<FetchRequestPtr> transfers;

  protected:
    /** Start fetching from a new address. */
    void changeStream(const BranchData &branch);

    /** Convert a response to a ForwardLineData */
    ForwardLineData* processResponse(FetchRequestPtr req);

    FetchRequestPtr initiateFetchLine(Addr fetchAddr);

  public:
    Fetch1(const std::string &name_,
        Cva6CPU &cpu_,
        const BaseCva6CPUParams &params,
        BranchData &resolved_branch_,
        ForwardLineDataReg & out_,
        BranchData &prediction_);

  public:
    /** Returns the IcachePort owned by this Fetch1 */
    Cva6CPU::Cva6CPUPort &getIcachePort() { return icachePort; }

    void onRecv(FetchRequestPtr req);
    /** Pass on input/buffer data to the output if you can */
    void evaluate();

    /** Flush the stage */
    void flush();

    /** Initiate fetch1 fetching */
    void wakeupFetch();
};

} // namespace cva6
} // namespace gem5
