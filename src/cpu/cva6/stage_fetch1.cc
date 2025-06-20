/**
 * @file stage_fetch1.cc
 * @author Pierre Ravenel (pravenel@kalrayinc.com)
 * @brief
 * @version 1.0
 * @date 2023-05-25
 *
 */


#include "cpu/cva6/stage_fetch1.hh"

#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/cva6/pipeline.hh"
#include "debug/Cva6Fetch.hh"
#include "debug/Cva6X.hh"

namespace gem5 {
namespace cva6 {

Fetch1::Fetch1(const std::string &name_,
        Cva6CPU &cpu_,
        const BaseCva6CPUParams &params,
        BranchData &resolved_branch_,
        ForwardLineDataReg & out_,
        BranchData &prediction_)  :
    Named(name_),
    cpu(cpu_),
    resolved_branch(resolved_branch_),
    out(out_),
    prediction(prediction_),
    icachePort(name_ + ".icache_port", *this, cpu_)
{
    // boot addr
    fetchInfo.pc.reset(params.isa[0]->newPCState());
    maxLineWidth = cpu.cacheLineSize();
}

Fetch1::FetchRequestPtr
Fetch1::initiateFetchLine(Addr fetchAddr)
{
    /* $ line
     *                 Request size
     *             <----------------->
     *  [XXXXXXXXXXXXXXXXXXXXXXXXXXXX]
     *  ^          ^
     *  |          |
     *  |          fetchAddr
     *  aligned_pc
     */
    Addr aligned_pc = fetchAddr & ~((Addr) maxLineWidth - 1);
    unsigned int request_size = maxLineWidth - (fetchAddr - aligned_pc);

    DPRINTF(Cva6Fetch, "fetchLine addr: 0x%x pc: %s request_size: %d\n",
        aligned_pc, fetchAddr, request_size);

    /* Create request */
    FetchRequestPtr request = new FetchRequest(cpu, *this, fetchAddr);
    request->request->setContext(cpu.thread->getTC()->contextId());
    request->request->setVirt(fetchAddr, request_size,
        Request::INST_FETCH, cpu.instRequestorId(), /* pc */ fetchAddr);

    return request;
}

void
Fetch1::changeStream(const BranchData &branch)
{
    DPRINTF(Cva6Fetch, "changeStream : %s\n", branch);
    /* Update the PC and the fetch addr */
    set(fetchInfo.pc, branch.squash_target);
    fetchInfo.fetchAddr = fetchInfo.pc->instAddr();

    if (branch.isStreamChange()){
        flush();
    }
}

ForwardLineData*
Fetch1::processResponse(Fetch1::FetchRequestPtr response){
    ForwardLineData* line_out = out.alloc();
    assert(response->isComplete());

    /* Set attributes */
    line_out->setFault(response->fault);
    set(line_out->pc, fetchInfo.pc);
    line_out->fetchAddr = response->pc;
    line_out->lineBaseAddr = response->request->getVaddr();

    if (response->fault == NoFault) {
        assert(!response->packet->isError());
        line_out->adoptPacketData(response->packet);
        /* Null the packet to prevent the response from
        * trying to deallocate the packet */
        response->packet = NULL;
    }
    return line_out;
}

void
Fetch1::onRecv(FetchRequestPtr req){
    assert(transfers.size());
    DPRINTF(Cva6Fetch, "Process %s", transfers.front()->name());
    assert(req == transfers.front());
    FetchRequestPtr ireq = req;
    ForwardLineData *line = processResponse(ireq);
    transfers.pop_front();
    ireq->untrack();
    out.push(line);
}

#define INFLIGHT 1
void
Fetch1::evaluate()
{
    // static BranchData &old_fetch_branch = BranchData::bubble();

    const BranchData &fetch2_branch = prediction;

    /* Are we changing stream?
     * (1) Look to the Execute branches first, then
     * (2) predicted changes of stream from Fetch2 */
    if (resolved_branch.isStreamChange()) {
        changeStream(resolved_branch);
    } else if (fetch2_branch.is_predicted) {
        changeStream(fetch2_branch);
    }

    /* (fetch@) -> Translate */
    if (requests.size() < INFLIGHT){ /* No other req in TLB */
        /* Generate fetch */
        FetchRequestPtr ireq = initiateFetchLine(fetchInfo.fetchAddr);
        ireq->translateTiming(); /* Translate */
        requests.push_back(ireq); /* Push */
        /* Step the PC for the next line onto the line aligned next address */
        fetchInfo.fetchAddr = fetchInfo.fetchAddr + ireq->request->getSize();
    }

    uint64_t inflights = transfers.size() + out.size();
    /* Translated ->  sendData */
    if (!requests.empty() &&                /* Request to process */
        requests.front()->isTranslated() && /* Request is translated */
        inflights < INFLIGHT &&             /* No other req in memory */
        !cpu.icache->isBlocked()            /* Cache ready */
    ){
        // Change queue (before send data because may spend 0cycle)
        FetchRequestPtr ireq = requests.front();
        requests.pop_front();
        transfers.push_back(ireq);
        // Send data
        ireq->sendData();
    }
}

void
Fetch1::flush(){
    for (auto &req: requests){
        req->untrack();
    }
    requests.clear();
    for (auto &req: transfers){
        req->untrack();
    }
    transfers.clear();
}

void
Fetch1::wakeupFetch() {
    ThreadContext *thread_ctx = cpu.getContext();
    set(fetchInfo.pc, thread_ctx->pcState());
    fetchInfo.fetchAddr = fetchInfo.pc->instAddr();
    DPRINTF(Cva6Fetch, "Changing stream wakeup %s\n", *fetchInfo.pc);
}


void
Fetch1::FetchRequest::translateTiming() {
    DPRINTF(Cva6X, "translateTiming...\n");
    assert(state == FetchRequest::NotIssued);
    state = FetchRequest::InTranslation;
    cpu.thread->mmu->translateTiming(request, cpu.getContext(),
        this, BaseMMU::Execute);
    /* wait finish() ... */
}

void
Fetch1::FetchRequest::finish(const Fault &fault_, const RequestPtr &request_,
                             ThreadContext *tc, BaseMMU::Mode mode) {
    assert(state == FetchRequest::InTranslation);
    if (!inuse){ /* Autodestruction */
        delete this;
        return;
    }
    fault = fault_;
    state = FetchRequest::Translated;
    if (fault_ != NoFault) {
        DPRINTF(Cva6X, "Fault in address ITLB translation: %s, "
            "paddr: 0x%x, vaddr: 0x%x\n",
            fault_->name(), (request->hasPaddr() ? request->getPaddr() : 0),
            request->getVaddr());
    } else {
        assert(request->hasPaddr());
        DPRINTF(Cva6X, "ITLB translation: PA: %x, VA: %x\n",
            request->getPaddr(), request->getVaddr());
    }
    // sendData();
}

void
Fetch1::FetchRequest::sendData() {
    DPRINTF(Cva6X, "sendData...\n");
    assert(state == FetchRequest::Translated);
    state = FetchRequest::RequestIssuing;

    /* Make the necessary packet for a memory transaction */
    packet = new Packet(request, MemCmd::ReadReq);
    packet->allocate();
    /* Allow the response to be identified */
    packet->pushSenderState(this);
    // Ensure that the packet won't delete the request
    assert(packet->needsResponse());

    if (fault != NoFault){ // TLB Fault: do not request cache
        packet->makeResponse();
        onRecv(packet);
        return;
    }

    if (!cpu.getInstPort().sendTimingReq(packet)) {
       fatal("Must sucess !");
    }
    /* Wait onRecv */
}

void
Fetch1::FetchRequest::onRecv(PacketPtr pkt) {
    if (!inuse){ /* Autodestruction */
        delete this;
        return;
    }
    DPRINTF(Cva6X, "Received fetch\n");
    assert(state == FetchRequest::RequestIssuing);

    if (fault == NoFault && pkt->isError()){
        fault = std::make_shared<SpeculativeFault>("Packet error\n");
    }
    // if (fault == NoFault){
    // In case of IT pkt may be error !
    //     assert(!pkt->isError());
    // }
    packet = pkt;
    state = Complete;

    /* fetch end of path */
    fetch.onRecv(this);
}

static std::string ITLBRequestStateName[] = {
    "____NotIssued",
    "InTranslation",
    "___Translated",
    "_____InMemory",
    "_____Complete"
};

static inline std::ostream & operator << (std::ostream &o,
    Fetch1::FetchRequest::FetchRequestState e) {
    assert((int)e < (int)Fetch1::FetchRequest::FetchRequestState::End);
    return o << ITLBRequestStateName[e];
}

std::string
Fetch1::FetchRequest::name(){
    std::ostringstream oss;
    oss << "<...>.ITLBreq("
        << state
        << std::hex
        << ",Vx" << request->getVaddr()
        << ",#x" << request->getSize()
        << ",[R]";
    //    << ",Dx";
    // assert(data);
    // if (req->getSize()==8) { oss << *(uint64_t*)data; }
    // else if (req->getSize()==4) { oss << *(uint32_t*)data; }
    // else if (req->getSize()==2) { oss << *(uint16_t*)data; }
    // else if (req->getSize()==1) { oss << *(uint8_t *)data; }
    // else                        { oss << "E"; }
    oss << ")";
    return oss.str();
}


Fetch1::FetchRequest::~FetchRequest() {
    if (packet)
        delete packet;
}


} // namespace cva6
} // namespace gem5
