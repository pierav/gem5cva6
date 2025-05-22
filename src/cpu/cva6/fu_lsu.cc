/**
 * @file fu_lsu.cc
 * @author Pierre Ravenel (pravenel@kalray.eu)
 * @brief
 * @version 0.1
 * @date 20/01/2023
 *
 */


#include "cpu/cva6/fu_lsu.hh"

#include <iomanip>
#include <sstream>
#include <typeinfo>

#include "cpu/cva6/pipeline.hh"
#include "debug/Cva6LSU.hh"

// #include "debug/Cva6Timing.hh"
// #include "enums/OpClass.hh"

#define PUSH_STR "initiate  "
#define SEND_STR "send data "
#define POP_STR  "complete  "
#define FW_STR   "forward   "

namespace gem5 {
namespace cva6 {

inline uint64_t basemask(Cva6DynInstPtr inst){
    uint64_t size = inst->dreq->getSize();
    return size >= sizeof(uint64_t) ? -1ULL : (1ULL << (size*8))-1;
}

inline uint64_t offsetDW(Cva6DynInstPtr inst){
    return (inst->dreq->getPaddr() & 0b111) * 8;
}

inline uint64_t makeMaskDW(Cva6DynInstPtr inst){
    return basemask(inst) << offsetDW(inst);
}

/************************************************************************
 * Store buffer
 ***********************************************************************/

bool maskMatchVaddrInst(Cva6DynInstPtr i1, Cva6DynInstPtr i2, uint64_t mask){
    return (i1->dreq->req->getVaddr() & mask) ==
           (i2->dreq->req->getVaddr() & mask);
}

bool
LSUStoreBuffer::isEmpty(){
    return (speculative_queue.size() == 0) && (commit_queue.size() == 0);
}

bool
LSUStoreBuffer::isNoPendingReqBefore(Cva6DynInstPtr& inst){
    for (auto &i2: speculative_queue){
        if (!i2->isAfterOrEqual(inst)){
            return false;
        }
    }
    for (auto &i2: commit_queue){
        if (!i2->isAfterOrEqual(inst)){
            return false;
        }
    }
    /* Finnaly return true */
    return true;
}

bool
LSUStoreBuffer::isMaskMatchVaddr(Cva6DynInstPtr inst, uint64_t mask){
    Addr addr_masked = inst->dreq->req->getVaddr() & mask;
    // Check if the page offset matches and whether the entry is valid,
    // for the commit queue
    for (Cva6DynInstPtr i2: commit_queue){
        if (i2->isAfterOrEqual(inst)){
            continue;
        }
        if ((i2->dreq->req->getVaddr() & mask) == addr_masked){
            DPRINTF(Cva6LSU, "* Match comitted %s : (%x)\n", *i2,
                i2->dreq->req->getVaddr());
            return 1;
        }
    }
    // do the same for the speculative queue
    for (Cva6DynInstPtr i2: speculative_queue){
        if (i2->isAfterOrEqual(inst)){
            continue;
        }
        if ((i2->dreq->req->getVaddr() & mask) == addr_masked){
            DPRINTF(Cva6LSU, "* Match speculative %s : (%x)\n", *i2,
                i2->dreq->req->getVaddr());
            return 1;
        }
    }
    return 0;
}

/* TODO: for now we are assuming InO queues ! */
uint64_t
LSUStoreBuffer::lookupSQDW(Cva6DynInstPtr inst, uint64_t& value){
    uint64_t paddr = inst->dreq->getPaddr();
    /* Do it from old to new */
    value = 0;
    uint64_t valid_mask = 0;
    uint64_t dwaddr = paddr & ~0b111ULL;
    for (Cva6DynInstPtr i2: commit_queue){
        if (!inst->isAfterOrEqual(i2)){ // Ignore futur instructuions
            continue;
        }
        if (dwaddr == i2->dreq->getDWPaddr()){
            DPRINTF(Cva6LSU, "MatchC %s:%s\n", *i2, i2->dreq->name());
            uint64_t cur_mask = makeMaskDW(i2);
            /* Update value and mask */
            value &= ~cur_mask; /* Remove old value */
            value |= i2->dreq->getDWData() & cur_mask; /* Set new value */
            valid_mask |= cur_mask; /* Update mask */
        }
    }
    for (Cva6DynInstPtr i2: speculative_queue){
        if (!inst->isAfterOrEqual(i2)){ // Ignore futur instructuions
            continue;
        }
        if (dwaddr == i2->dreq->getDWPaddr()){
            DPRINTF(Cva6LSU, "MatchS %s:%s\n", *i2, i2->dreq->name());
            uint64_t cur_mask = makeMaskDW(i2);
            /* Update value and mask */
            value &= ~cur_mask; /* Remove old value */
            value |= i2->dreq->getDWData() & cur_mask; /* Set new value */
            valid_mask |= cur_mask; /* Update mask */
        }
    }
    return valid_mask;
}

bool
LSUStoreUnit::lookupSQ(Cva6DynInstPtr inst){
    uint64_t value = 0;
    uint64_t valid_mask = store_buffer.lookupSQDW(inst, value);
    /* Normalise */
    valid_mask = (valid_mask >> offsetDW(inst)) & basemask(inst);
    value = (value >> offsetDW(inst)) & basemask(inst);

    /* Forward something but not everything */
    if ((valid_mask & basemask(inst)) == basemask(inst)){
        DPRINTF(Cva6LSU, "Forward Full : %lx: mask=%lx\n", value, valid_mask);
        inst->dreq->complete_forward(value);
        return true;
    }
    /* Adjust dreq to fit inflights */
    if (valid_mask){
        DPRINTF(Cva6LSU, "Forward partial : %lx : mask=%lx\n",
            value, valid_mask);
        /* Only read unsets bytes */
        // CANNOT WORKS "be" is only used to perform sparse write
        // std::vector<bool> be;
        // be.resize(inst->dreq->getSize());
        // for (int i = 0; i < inst->dreq->getSize(); i++){
        //   be[i] = (valid_mask >> (i * 8)) & 1 ? false : true;
        // }
        // inst->dreq->req->setByteEnable(be);
        /* Set value in payload (debug) */
        inst->dreq->setRawData(value);
        /* Setup forward flags */
        inst->dreq->fwval = value;
        inst->dreq->fwmask = valid_mask;
    }
    return false;
}

bool
LSUStoreBuffer::canPush(Cva6DynInstPtr inst_){
    return speculative_queue.size() < depth_spec;
}

void
LSUStoreBuffer::push(Cva6DynInstPtr inst_){
        assert(canPush(inst_));
    speculative_queue.push(inst_);
    DPRINTF(Cva6LSU, PUSH_STR "%s %s\n", *inst_, inst_->dreq->name());
}

void
LSUStoreBuffer::flushfrom(Cva6DynInstPtr inst_){
    /* Only flush speculative */
    // while (!speculative_queue.empty() &&
    //     speculative_queue.back()->isAfterOrEqual(inst_)){
    //     Cva6DynInstPtr inst = speculative_queue.back();
    //     speculative_queue.pop_back();
    //     inst->untrackDreq();
    // }

    // while (!commit_queue.empty() &&
    //     !commit_queue.back()->commit_completed &&
    //     commit_queue.back()->isAfterOrEqual(inst_)){
    //     Cva6DynInstPtr inst = commit_queue.back();
    //     commit_queue.pop(inst);
    //     inst->untrackDreq();
    // }
    speculative_queue.flushfrom(inst_);
    commit_queue.flushfrom(inst_);


}

bool
LSUStoreBuffer::canPop(Cva6DynInstPtr inst_){
    // if (!speculative_queue.empty() && speculative_queue.front() == inst_){
    //     DPRINTF(Cva6LSU, "STROREB %s\n", inst_->dreq->name());
    // }
    return !speculative_queue.empty() && /* data to commit */
            inst_->dreq->isTranslated() && /* */
            commit_queue.size() < depth_commit; /* Available space*/
}

void
LSUStoreBuffer::pop(Cva6DynInstPtr inst_){
    DPRINTF(Cva6LSU, "mv %s from speculative to commit queue\n", *inst_);
    assert(canPop(inst_));
    assert(speculative_queue.size() > 0);
    assert(commit_queue.size() < depth_commit);
    // only for InO:
    // Cva6DynInstPtr inst = speculative_queue.front();
    // assert(inst == inst_);
    speculative_queue.pop(inst_);
    commit_queue.push(inst_);
}

bool
LSUStoreBuffer::advance(){

    // static int pop_lat = 0;
    DPRINTF(Cva6LSU, "Advance... [%d]->[%d]\n", speculative_queue.size(),
        commit_queue.size());
    for (Cva6DynInstPtr inst: commit_queue){
        DPRINTF(Cva6LSU, "SQC: %s %s\n", *inst, inst->dreq->name());
    }
    /** send store_buffer & commit_queue to memory */
    /* Received data ? -> pop */
    if (!commit_queue.empty()){
        Cva6DynInstPtr inst = commit_queue.front();
        DTLBRequestPtr dreq = inst->dreq;
        if (inst->dreq->isCompleted()){
            DPRINTF(Cva6LSU, POP_STR "%s %s\n", *inst, dreq->name());
            commit_queue.pop(inst);
        }
    }

    /* Translated ? -> send */
    for (Cva6DynInstPtr inst: commit_queue){
        DTLBRequestPtr dreq = inst->dreq;
        assert(dreq);
        if (dreq->isInMemory()){
            continue;
        }
        if (inst->commit_completed && // No more speculative
            inst->dreq->isTranslated() && // Has Paddr
            !cpu.dcache->isBlocked()) // Cache ready
        {
            inst->dreq->sendData();
            // break;
        }
    }

    return !commit_queue.empty();
}


/************************************************************************
 * Amo buffer Unit
 ***********************************************************************/

bool
LSUAmoBuffer::canPush(Cva6DynInstPtr inst_){
    return amo_buffer->isBubble();
}

void
LSUAmoBuffer::push(Cva6DynInstPtr inst_){
    assert(canPush(inst_));
    /* Push */
    DPRINTF(Cva6LSU, PUSH_STR "%s %s\n", *inst_, inst_->dreq->name());
    amo_buffer = inst_;
}

/** Can Commit */
bool
LSUAmoBuffer::canPop(Cva6DynInstPtr inst_){
    /* Must be true */
    if (amo_buffer->isBubble() || amo_buffer != inst_){
        DPRINTF(Cva6LSU, "canPop %s not present", *inst_);
        return false;
    }
    /* Not completed */
    if (!amo_buffer->dreq->isCompleted()){
        DPRINTF(Cva6LSU, "canPop %s %s", *inst_, amo_buffer->dreq->name());
        return false;
    }
    return true;
}

/** Commit instruction */
void
LSUAmoBuffer::pop(Cva6DynInstPtr inst_){
    assert(canPop(inst_));

    DPRINTF(Cva6LSU, POP_STR "%s %s\n", *amo_buffer,
            amo_buffer->dreq->name());
    amo_buffer = Cva6DynInst::bubble();
    amo_committed = false;
}

void
LSUAmoBuffer::flushfrom(Cva6DynInstPtr inst_){
    /* Only flush speculative */
    // fatal_if(amo_committed, "Cannot flush commited amo: %s\n",
    //     *amo_buffer);
    if (!amo_buffer->isBubble() &&
        !amo_committed){
        if (amo_buffer->isAfterOrEqual(inst_)){
            amo_buffer->untrackDreq();
            amo_buffer = Cva6DynInst::bubble();
        }
    }
}

bool
LSUAmoBuffer::advance(){
    if (amo_buffer->isBubble()){
        DPRINTF(Cva6LSU, "Advance...\n");
    } else {
        DPRINTF(Cva6LSU, "Advance... store_buffer()=%d %s %s\n",
        store_buffer->isNoPendingReqBefore(amo_buffer),
        *amo_buffer, amo_buffer->dreq->name());
    }

    /* Need to send memory request ? */
    if (!amo_buffer->isBubble() && /* Data to transfer */
        store_buffer->isNoPendingReqBefore(amo_buffer) &&
        /* All stores have drained */
        cpu.pipeline->isCommitInst(amo_buffer) &&
        /* The AMO is in the commit stage */
        amo_buffer->dreq->isTranslated() && /* Not already sent*/
        !cpu.dcache->isBlocked() /* Cache ready */
    ){
        amo_committed = true;
        amo_buffer->dreq->sendData();
        DPRINTF(Cva6LSU, SEND_STR "%s %s\n", *amo_buffer,
                amo_buffer->dreq->name());
    }
    return !amo_buffer->isBubble();
}


bool
LSUAmoBuffer::isMaskMatchVaddr(Cva6DynInstPtr inst, uint64_t mask){
    if (amo_buffer->isBubble()){
        return false;
    }
    if (amo_buffer->isAfterOrEqual(inst)){
        return false;
    }
    Addr t0 = inst->dreq->req->getVaddr() & mask;
    Addr t1 = amo_buffer->dreq->req->getVaddr() & mask;
    return t1 == t0; // Return if addr masked match
}

/************************************************************************
 * Load Unit
 ***********************************************************************/

bool
LSULoadUnit::canPush(Cva6DynInstPtr inst_){
    /** Check collision with store unit (SQ and amo buffer) */
    return loadqueue.size() < 8 // TODO! Cannot be one day true
            && !su->isPageOffsetMatches(inst_); //
}

void
LSULoadUnit::push(Cva6DynInstPtr inst_){
    assert(canPush(inst_));
    /* Push */
    DPRINTF(Cva6LSU, PUSH_STR "%s %s\n", *inst_, inst_->dreq->name());
    loadqueue.push(inst_);
    /* Annotate scheduling */
    inst_->dreq->is_cl_req_inorder = !su->isClMatch(inst_,
        DTLBRequest::BASESIZE);
    stats.req += 1;
    stats.oooreq += !inst_->dreq->is_cl_req_inorder;
}

bool
LSULoadUnit::canPop(Cva6DynInstPtr inst_){
    /** Decoupled retire load */
    /** wait TLB & mem packet */
    return insts_in_memory.canPop(inst_) &&
           inst_->dreq->isCompleted();
}

void
LSULoadUnit::pop(Cva6DynInstPtr inst_){
    assert(canPop(inst_));
    DPRINTF(Cva6LSU, POP_STR "%s %s\n", *inst_, inst_->dreq->name());
    insts_in_memory.pop(inst_);
}

void
LSULoadUnit::flushfrom(Cva6DynInstPtr inst_){
    loadqueue.flushfrom(inst_);
    insts_in_memory.flushfrom(inst_);
    //assert(inst_in_memory->isBubble()); // Load are not speculative
    // if (!inst_in_memory->isBubble()){
    //     inst_in_memory->untrackDreq();
    // }
    // inst_in_memory = Cva6DynInst::bubble(); // PR: DELETE!!!
}

bool
LSULoadUnit::advance(){
    DPRINTF(Cva6LSU, "Advance... [%d]\n", loadqueue.size());
    /* Try to send instruction to memory */

    while (!loadqueue.empty()){
        Cva6DynInstPtr inst = loadqueue.front();
        DPRINTF(Cva6LSU, "Select Queue : %s : %s\n", *inst,
            inst->dreq->name());

        /* Instruction is translated */
        if (!inst->dreq->isTranslated()){ /* Must be true one day */
            break;
        }
        /* Cache is ready */
        if (!insts_in_memory.canPush()){ /* Always true */
            break;
        }
        if (cpu.dcache->isBlocked()){ /* Must be true one day */
            break;
        }
        /* If inst is uncacheable, wait inst until commit head */
        if (inst->dreq->req->isUncacheable()){
            if (!cpu.pipeline->isCommitInst(inst)){
                /* Cannot be true one day ! Solution is to replay */
                // break; // OLD !
                // NEW: Mark a replay fault
                uint64_t pc = inst->pc->instAddr();
                inst->dreq->fault = std::make_shared<FlushBeforeFault>(pc);
            }
        }

        /* When all conditions are met, send data */
        inst->dreq->sendData();
        // Also move instructuction to allow new space
        insts_in_memory.push(inst);
        loadqueue.pop(inst);
        break;
    }

    return !loadqueue.empty();
}

/************************************************************************
 * Load Store Unit
 ***********************************************************************/

bool
LSUBase::canPush(Cva6DynInstPtr inst){
    return lsu_fifo.size() < 8;
}

#include "arch/riscv/pma_checker.hh"

void
LSUBase::push(Cva6DynInstPtr inst){
    assert(inst->dreq);
    lsu_fifo.push(inst);
    /* DO the translation here to overlap cache latency
     * and TLB hit latency. In this way, the Load to use is
     * equal to the cache latency setup in .py file */
    // UNDO
}

bool
LSUBase::canPop(Cva6DynInstPtr inst){
    if (inst->dreq->fault != NoFault){
        return true;
    } else {
        return destUnit(inst)->canPop(inst);
    }
}

void
LSUBase::pop(Cva6DynInstPtr inst){
    if (inst->dreq->fault != NoFault){
        // Nothing to do
    } else {
        destUnit(inst)->pop(inst);
    }
}

void
LSUBase::flushfrom(Cva6DynInstPtr inst_){
    load_unit.flushfrom(inst_);
    store_unit.flushfrom(inst_);
    lsu_fifo.flushfrom(inst_);
}

bool
LSUBase::advance(){

    DPRINTF(Cva6LSU, "Advance ... [%d]\n", lsu_fifo.size());
    auto it = lsu_fifo.begin();
    while (it != lsu_fifo.end()){
        Cva6DynInstPtr &inst = *it;
        // assert(inst->dreq->fault == NoFault);
        if (!inst->dreq->isLaunched()){
            inst->dreq->translateTiming();
        }
        if (!inst->dreq->isTranslated()){
            DPRINTF(Cva6LSU, "In translation : %s \n",
                *inst, inst->dreq->name());
            it++;
            continue;
        }
        if (inst->dreq->fault != NoFault){
            DPRINTF(Cva6LSU, "issue fault    : %s \n",
                *inst, inst->dreq->name());
            it=lsu_fifo.erase(it);
            continue;
        }
        assert(inst->dreq->req->hasPaddr());
        /* Amo must be drained */
        if (store_unit.amo_buffer.isPageOffsetMatches(inst)){
            DPRINTF(Cva6LSU, "Break on amo   : %s \n",
                *inst, inst->dreq->name());
            break;
        }
        /* Drain to avoid store -> load */
        bool isStoreBefore = false;
        for (auto &i2: lsu_fifo){
            if (i2 == inst){
                break;
            }
            if (!i2->isFault() && !i2->staticInst->isLoad()){
                isStoreBefore = true;
                break;
            }
        }
        /* Store serialisation */
        if (!inst->staticInst->isLoad()){
            if (isStoreBefore){
                DPRINTF(Cva6LSU, "Break on store : %s \n",
                    *inst, inst->dreq->name());
                break;
            }
        }
        /* Load bypass */
        if (inst->staticInst->isLoad() &&
           lsu_fifo.isPageOffsetMatches(inst)){
            DPRINTF(Cva6LSU, "Break on deps  : %s \n",
                *inst, inst->dreq->name());
            break;
        }
        if (destUnit(inst)->canPush(inst)){
            DPRINTF(Cva6LSU, "Issue          : %s \n",
                *inst, inst->dreq->name());
            /* Push inst */
            destUnit(inst)->push(inst);
            /* Mark MDP checker */
            cpu.pipeline->mdpc.issue(inst);
            it=lsu_fifo.erase(it);
            continue;
        } else {
            DPRINTF(Cva6LSU, "Fu Stall for   : %s \n",
                *inst, inst->dreq->name());
            it++;
            continue;
        }
    }
    store_unit.advance();
    load_unit.advance();
    return true;
}

/************************************************************************
 * Load Store Unit (Checker)
 ***********************************************************************/
bool
LSUBaseChecker::canPush(Cva6DynInstPtr inst){
    return lsu_fifo.size() < 8;
}

void
LSUBaseChecker::push(Cva6DynInstPtr inst){
    assert(inst->dreq);
    lsu_fifo.push(inst);
}

bool
LSUBaseChecker::canPop(Cva6DynInstPtr inst){
    if (inst->dreq->fault != NoFault){
        return true;
    } else {
        return inst->dreq->isCompleted();
    }
}

void
LSUBaseChecker::pop(Cva6DynInstPtr inst){
    /* Perform checks */
    if (inst->dreq->fault != NoFault){
        return; /* Nothing to do */
    }
    assert(inst->dreq->is_prefetch_mode);
    if (!inst->staticInst->isLoad()){
        uint64_t prefetch_data = inst->dreq->getData();
        // FUCK ME !!!!!!!!! Addr is 0 !!!
        uint64_t data = inst->getSrcRegOperand(1); // Data or addr ?
        uint16_t size = inst->dreq->getSize();
        bool misspred = memcmp(&data, &prefetch_data, size) != 0;
        if (inst->dreq->prefetch_mode_failed || misspred){
            inst->dreq->prefetch_mode_failed = true;
        }
    }
}

void
LSUBaseChecker::flushfrom(Cva6DynInstPtr inst_){
    cqueue.flushfrom(inst_);
    lsu_fifo.flushfrom(inst_);
}

bool
LSUBaseChecker::advance(){
    /* Perform translations */
    DPRINTF(Cva6LSU, "Advance ... [%d]\n", lsu_fifo.size());
    for (Cva6DynInstPtr inst: lsu_fifo){
        if (!inst->dreq->isLaunched()){
            inst->dreq->translateTiming();
        }
    }

    /* */
    while (!lsu_fifo.empty()){
        Cva6DynInstPtr inst = lsu_fifo.front();
        if (!inst->dreq->isTranslated()){
            break;
        }

        /* Push in CQ */
        if (inst->dreq->fault != NoFault){
            lsu_fifo.pop(inst);
            continue;
        } else {
            if (cqueue.size() < 8){
                cqueue.push(inst);
                lsu_fifo.pop(inst);
                continue;
            }
        }
        break; // Break if no access emitted
    }

    /* Try to send instruction to memory */
    while (!cqueue.empty()){
        Cva6DynInstPtr inst = cqueue.front();
        /* Instruction is translated */
        if (!inst->dreq->isTranslated()){
            DPRINTF(Cva6LSU, "Stall : Not Translated : %s : %s\n", *inst,
                inst->dreq->name());
            break;
        }

        /* Check order correctness */
        if (su->isPageOffsetMatches(inst)){
            DPRINTF(Cva6LSU, "Stall : SU match ......: %s : %s\n", *inst,
                inst->dreq->name());
            break;
        }

        if (cpu.dcache->isBlocked()){
            DPRINTF(Cva6LSU, "Stall : Cache blocked .: %s : %s\n", *inst,
                inst->dreq->name());
            break;
        }

        /* Switch store to load and load to valid loads */
        inst->dreq->setPrefetchMode();
        /* When all conditions are met, send data */
        inst->dreq->sendData();
        /* Also move instructuction to allow new space */
        cqueue.pop(inst);
        break;
    }
    return true;
}

void
LSULoadUnitNoLock::push(Cva6DynInstPtr inst){
    /* Preliminary check for loads */
    if (inst->dreq->req->isUncacheable()){
        if (!cpu.pipeline->isCommitInst(inst)){
            uint64_t pc = inst->pc->instAddr();
            inst->dreq->fault = std::make_shared<FlushBeforeFault>(pc);
            inst->dreq->sendData();
            return;
        }
    }
    /* Forward from SQ if possible */
    if (su->lookupSQ(inst)){
        assert(inst->dreq->isCompleted());
        stats.full_forward += 1;
        return; /* Req completed */
    }
    /* Finally read data from cache */
    assert(!cpu.dcache->isBlocked()); /* Cache ready */
    stats.no_forward += !inst->dreq->req->isMasked();
    stats.partial_forward += inst->dreq->req->isMasked();
    inst->dreq->sendData();
    return;
}

} // namespace cva6
} // namespace gem5
