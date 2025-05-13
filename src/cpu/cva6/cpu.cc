#include "cpu/cva6/cpu.hh"

#include "arch/riscv/faults.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/pipeline.hh"
#include "debug/Drain.hh"
#include "debug/Quiesce.hh"

namespace gem5 {
namespace cva6 {
} // namespace cva6

void
DTLBRequest::setupfault() {

    /* Cva6 restriction: Check alignement */
    if (req->getVaddr() % req->getSize() != 0){
    RiscvISA::ExceptionCode code;
    if (mode == BaseMMU::Read){
        code = RiscvISA::ExceptionCode::LOAD_ADDR_MISALIGNED;
    } else if (mode == BaseMMU::Write) {
        code = RiscvISA::ExceptionCode::STORE_ADDR_MISALIGNED;
    } else {
        fatal("?");
    }
    fault = std::make_shared<RiscvISA::AddressFault>(req->getVaddr(), code);
    // fault = std::make_shared<SpeculativeFault>("bad align");
    } else {
    // assert((addr & 0b111) + size <= 8);
    }
}

void
DTLBRequest::translateTiming(){
    DPRINTF(Cva6X, "translateTiming...\n");
    assert(state == NotIssued);
    state = InTranslation;
    cpu.thread->mmu->translateTiming(req, cpu.thread->getTC(), this, mode);
    /* wait finish() ... */
}

void
DTLBRequest::translateFunctional(){
    DPRINTF(Cva6X, "translateFunctional...\n");
    assert(state == NotIssued);
    cpu.thread->mmu->translateTiming(req, cpu.thread->getTC(), this, mode);
}


void
DTLBRequest::finish(const Fault &fault_, const RequestPtr &request_,
            ThreadContext *tc, BaseMMU::Mode mode){
    if (state == NotIssued){ /* Functionnal access */
        return;
    }
    DPRINTF(Cva6X, "DTLB finish()\n");

    assert(state == InTranslation);
    assert(request_ == req);
    if (!inuse){ /* Autodestruction */
        delete this;
        return;
    }
    if (fault == NoFault){
        fault = fault_;
    }
    state = Translated;

    if (fault != NoFault) {
        DPRINTF(Cva6X, "DTLB translation Fault: %s PA: %x, VA: %x\n",
            fault->name(), (req->hasPaddr() ? req->getPaddr() : 0),
            req->getVaddr());
    } else {
        DPRINTF(Cva6X, "DTLB translation: PA: %x, VA: %x\n",
            req->getPaddr(), req->getVaddr());
    }
    // sendData();
}

void
DTLBRequest::sendData(){
    DPRINTF(Cva6X, "sendData...\n");
    assert(state == Translated);
    state = InMemory;
    bool read = mode == BaseMMU::Read;
    if (is_prefetch_mode){
        read = true;
    }
    /* Build requested packed */
    pkt = read ? Packet::createRead(req) : Packet::createWrite(req);
    pkt->dataStatic<uint8_t>(data);
    pkt->pushSenderState(this);

    /* Early exit when Store Prefetch failure */
    if (is_prefetch_mode && !isBufferable()){
        prefetch_mode_failed = true;
        pkt->makeResponse();
        onRecv(pkt);
        return;
    }

    /* TLB Fault: do not request cache */
    if (fault != NoFault){
        pkt->makeResponse();
        // printf("FAULT!\n");
        onRecv(pkt);
        return;
    }

    #if 0
    /* If possible fetch cache line */
    if (mode == BaseMMU::Read &&
        is_cl_req_inorder &&
       !req->isUncacheable() &&
       !req->isAtomic() &&
       !req->isLLSC() &&
       !req->isSwap()&&
       !req->isCacheInvalidate() &&
       !req->isCacheClean() &&
       !req->isLockedRMW())
    { /* Create Paddr Cache line request */
        // printf("flags: %lx\n", req->getFlags() );
        // assert(req->getFlags() == 0); !!! Care Request::PHYSICAL
        DPRINTF(Cva6X, "request CL !\n");
        req2 = std::make_shared<Request>(
            getClPaddr(), getClSize(), 0, cpu.thread->contextId()
        );
        req2->setPC(req->getPC()); // Set PC for prefetcher
        // printf("CL for P:%lx V:%lx\n", getClPaddr(), getClVaddr());
        // TODO
        // if (req->isMasked()){ /* Propagate mask */
        //     std::vector<bool> be(getClSize());
        //     for (int i = 0; i < getClSize(); i++){
        //         bool in_req = TODO
        //     }
        // MUST CARE ABOUT NonForwarded Values

        pkt2 = new Packet(req2, MemCmd::ReadReq);
        pkt2->dataStatic<uint8_t>(getClData()); /* set data */
        pkt2->pushSenderState(this);
        /* allow the response to be identified */
        if (!cpu.getDataPort().sendTimingReq(pkt2)) {
            assert(0);
        }
        return;
        /* Wait onRecv() ... */
    }
    #endif

    /* Send packet */
    bool do_access = true;
    if (req->getFlags().isSet(Request::NO_ACCESS)) {
        pkt->makeResponse();
        onRecv(pkt);
        return;
    } else if (read) {
        if (req->isLLSC()) {
            cpu.thread->getIsaPtr()->handleLockedRead(req);
        }
    } else {
        if (req->isLLSC()) {
            do_access = cpu.thread->getIsaPtr()->handleLockedWrite(
                    req, ~(cpu.cacheLineSize() - 1));
            do_access = true;
        } else if (req->isCondSwap()) {
            assert(res);
            req->setExtraData(*res);
        }
    }
    if (do_access){
        if (!cpu.getDataPort().sendTimingReq(pkt)) {
            assert(0);
        }
        cpu.thread->getIsaPtr()->handleLockedSnoop(pkt,
            ~(cpu.cacheLineSize() - 1));
    } else {
        assert(0);
        onRecv(pkt);
    }
    /* Wait onRecv() ... */
}

void
DTLBRequest::onRecv(PacketPtr pkt_){
    assert(state == InMemory);
    if (!inuse){
        // Autodestruction
        delete this;
        return;
    }
    /* Perform FW if needed */
    if (fwmask){
        DPRINTF(Cva6X, "Forward %lx : mask=%lx\n", fwval, fwmask);
        uint64_t newval = (getData() & ~fwmask) | fwval;
        setRawData(newval);
    }
    DPRINTF(Cva6X, "Received load/store response\n");
    state = Complete;
}

void
DTLBRequest::complete_forward(uint64_t val){
    DPRINTF(Cva6X, "complete_forward...\n");
    // assert(state <= Translated);
    state = InMemory;
    assert(mode == BaseMMU::Read);
    assert(data);
    bool read = mode == BaseMMU::Read;
    pkt = read ? Packet::createRead(req) : Packet::createWrite(req);
    /* set data */
    pkt->dataStatic<uint8_t>(data);
    pkt->makeResponse();
    assert(pkt->getPtr<uint8_t>() == data);
    assert(req->getSize() == pkt->getSize());
    memcpy(pkt->getPtr<uint8_t*>(), &val, req->getSize());
    // Finish
    onRecv(pkt);
}

RegVal
DTLBRequest::getData(){
    if (req->getSize()==8) { return *(uint64_t*)data; }
    else if (req->getSize()==4) { return *(uint32_t*)data; }
    else if (req->getSize()==2) { return *(uint16_t*)data; }
    else if (req->getSize()==1) { return *(uint8_t *)data; }
    else                        { fatal("Invalid size\n"); }
    return 0;
}

std::string
DTLBRequest::name(){
    std::ostringstream oss;
    char key = mode == BaseMMU::Read ? 'R' :
                     req->isAtomic() ? 'A' : 'W';
    if (is_prefetch_mode){
        key = 'C';
    }
    oss << "<...>.DTLBreq("
        << state.str()
        << std::hex
        << ",Vx" << req->getVaddr()
        << ",#x" << req->getSize()
        << ",[" << key << "]"
        << ",Dx" << getData();
    oss << ")";
    return oss.str();
}

/** Dcache port */
bool
Cva6CPU::DcachePort::recvTimingResp(PacketPtr pkt){
    DTLBRequestPtr dreq = safe_cast<DTLBRequestPtr>(pkt->popSenderState());
    dreq->onRecv(pkt);
    return true;
}

void
Cva6CPU::DcachePort::recvTimingSnoopReq(PacketPtr pkt) {
    /* LLSC operations in CVA6 can't be speculative and are executed from
    * the head of the requests queue.  We shouldn't need to do more than
    * this action on snoops. */
    Addr cacheBlockMask = ~(cpu.cacheLineSize() - 1);
    if (pkt->isInvalidate() || pkt->isWrite()) {
        BaseISA* isa = cpu.getContext()->getIsaPtr();
        isa->handleLockedSnoop(pkt, cacheBlockMask);
    }
}


Cva6CPU::Cva6CPU(const BaseCva6CPUParams &params) :
    BaseCPU(params),
    dcachePort(name() + ".dcachePort", *this),
    dcache(params.dcache),
    icache(params.icache),
    stats(this)
{
    // fatal_if(dcache->isBlocked());
    assert(numThreads == 1);
    if (FullSystem) {
        thread = new cva6::Cva6Thread(this, 0, params.system,
                params.mmu, params.isa[0], params.decoder[0]);
        thread->setStatus(ThreadContext::Halted);
    } else {
        thread = new cva6::Cva6Thread(this, 0, params.system,
                params.workload[0], params.mmu,
                params.isa[0], params.decoder[0]);
    }

    ThreadContext *tc = thread->getTC();
    threadContexts.push_back(tc);

    if (params.checker) {
        fatal("The Cva6 model doesn't support checking (yet)\n");
    }

    pipeline = new cva6::Pipeline(*this, params);
}

Cva6CPU::~Cva6CPU()
{
    delete pipeline;
    delete thread;
}

void
Cva6CPU::init()
{
    BaseCPU::init();

    if (!params().switched_out && system->getMemoryMode() != enums::timing) {
        fatal("The Cva6 CPU requires the memory system to be in "
            "'timing' mode.\n");
    }
}

/** Stats interface from SimObject (by way of BaseCPU) */
void
Cva6CPU::regStats()
{
    BaseCPU::regStats();
    pipeline->regStats();
}

void
Cva6CPU::serializeThread(CheckpointOut &cp, ThreadID thread_id) const
{
    assert(thread_id == 0);
    thread->serialize(cp);
}

void
Cva6CPU::unserializeThread(CheckpointIn &cp, ThreadID thread_id)
{
    assert(thread_id == 0);
    thread->unserialize(cp);
}

void
Cva6CPU::serialize(CheckpointOut &cp) const
{
    pipeline->serialize(cp);
    BaseCPU::serialize(cp);
}

void
Cva6CPU::unserialize(CheckpointIn &cp)
{
    pipeline->unserialize(cp);
    BaseCPU::unserialize(cp);
}

void
Cva6CPU::wakeup(ThreadID tid)
{
    DPRINTF(Drain, "[tid:%d] Cva6CPU wakeup\n", tid);
    assert(tid == 0);

    if (thread->status() == ThreadContext::Suspended) {
        thread->activate();
    }
}

void
Cva6CPU::startup()
{
    BaseCPU::startup();
    pipeline->wakeupFetch();
}

DrainState
Cva6CPU::drain()
{
    // Deschedule any power gating event (if any)
    deschedulePowerGatingEvent();

    if (switchedOut()) {
        DPRINTF(Drain, "Cva6 CPU switched out, draining not needed.\n");
        return DrainState::Drained;
    }

    DPRINTF(Drain, "Cva6CPU drain\n");

    /* Need to suspend all threads and wait for Execute to idle.
     * Tell Fetch1 not to fetch */
    if (pipeline->drain()) {
        DPRINTF(Drain, "Cva6CPU drained\n");
        return DrainState::Drained;
    } else {
        DPRINTF(Drain, "Cva6CPU not finished draining\n");
        return DrainState::Draining;
    }
}

void
Cva6CPU::signalDrainDone()
{
    DPRINTF(Drain, "Cva6CPU drain done\n");
    Drainable::signalDrainDone();
}

void
Cva6CPU::drainResume()
{
    /* When taking over from another cpu make sure lastStopped
     * is reset since it might have not been defined previously
     * and might lead to a stats corruption */
    pipeline->resetLastStopped();

    if (switchedOut()) {
        DPRINTF(Drain, "drainResume while switched out.  Ignoring\n");
        return;
    }

    DPRINTF(Drain, "Cva6CPU drainResume\n");

    if (!system->isTimingMode()) {
        fatal("The Cva6 CPU requires the memory system to be in "
            "'timing' mode.\n");
    }

    wakeup(0);

    // Reschedule any power gating event (if any)
    schedulePowerGatingEvent();
}

void
Cva6CPU::memWriteback()
{
    DPRINTF(Drain, "Cva6CPU memWriteback\n");
}

void
Cva6CPU::switchOut()
{
    assert(!switchedOut());
    BaseCPU::switchOut();
}

void
Cva6CPU::takeOverFrom(BaseCPU *old_cpu)
{
    BaseCPU::takeOverFrom(old_cpu);
}

void
Cva6CPU::activateContext(ThreadID thread_id)
{
    assert(thread_id == 0);
    /* Do some cycle accounting.  lastStopped is reset to stop the
     *  wakeup call on the pipeline from adding the quiesce period
     *  to BaseCPU::numCycles */
    stats.quiesceCycles += pipeline->cyclesSinceLastStopped();
    pipeline->resetLastStopped();

    /* Wake up the thread, wakeup the pipeline tick */
    thread->activate();
    pipeline->start();

    BaseCPU::activateContext(0);
}

void
Cva6CPU::suspendContext(ThreadID thread_id)
{
    assert(thread_id == 0);

    thread->suspend();
    BaseCPU::suspendContext(0);
}

RequestPort &
Cva6CPU::getInstPort()
{
    return pipeline->getInstPort();
}

Counter
Cva6CPU::totalInsts() const
{
    return thread->numInst;
}

Counter
Cva6CPU::totalOps() const
{
    return thread->numOp;
}



} // namespace gem5
