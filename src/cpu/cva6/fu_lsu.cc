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

#include "base/named.hh"
#include "debug/Cva6LSU.hh"
#include "debug/Cva6Minicache.hh"

// #include "debug/Cva6Timing.hh"
// #include "enums/OpClass.hh"

#define PUSH_STR "initiate  "
#define SEND_STR "send data "
#define POP_STR  "complete  "
#define FW_STR   "forward   "

namespace gem5 {
namespace cva6 {

/************************************************************************
 * Store buffer
 ***********************************************************************/

bool
LSUStoreBuffer::isEmpty(){
    return (speculative_queue.size() == 0) && (commit_queue.size() == 0);
}

bool LSUStoreBuffer::isMaskMatchVaddr(Cva6DynInstPtr inst, uint64_t mask){
    Addr addr_masked = inst->dreq->req->getVaddr() & mask;
    // Check if the page offset matches and whether the entry is valid,
    // for the commit queue
    for (Cva6DynInstPtr isnt: commit_queue){
        if ((isnt->dreq->req->getVaddr() & mask) == addr_masked){
            return 1;
        }
    }
    // do the same for the speculative queue
    for (Cva6DynInstPtr isnt: speculative_queue){
        if ((isnt->dreq->req->getVaddr() & mask) == addr_masked){
            return 1;
        }
    }
    return 0;
}

bool
LSUStoreBuffer::isPageOffsetMatches(Cva6DynInstPtr inst){
    return isMaskMatchVaddr(inst, 0b111111111000);
}

bool
LSUStoreBuffer::isClMatch(Cva6DynInstPtr inst, uint64_t clsize){
    uint64_t mask = ((1 << 12) - 1); // 0b111111111111;
    mask &= ~(clsize - 1); // 0b111111110000
    // assert(mask == 0b111111110000);
    return isMaskMatchVaddr(inst, mask);
}

bool
LSUStoreBuffer::canPush(Cva6DynInstPtr inst_){
    return speculative_queue.size() < depth_spec;
}

void
LSUStoreBuffer::push(Cva6DynInstPtr inst_){
        assert(canPush(inst_));
    speculative_queue.push_back(inst_);
    DPRINTF(Cva6LSU, PUSH_STR "%s %s\n", *inst_, inst_->dreq->name());
}

void
LSUStoreBuffer::flushfrom(Cva6DynInstPtr inst_){
    /* Only flush speculative */
    while (!speculative_queue.empty() &&
        speculative_queue.back()->isAfterOrEqual(inst_)){
        Cva6DynInstPtr inst = speculative_queue.back();
        speculative_queue.pop_back();
        inst->untrackDreq();
    }

    while (!commit_queue.empty() &&
        !commit_queue.back()->commit_completed &&
        commit_queue.back()->isAfterOrEqual(inst_)){
        Cva6DynInstPtr inst = commit_queue.back();
        commit_queue.pop(inst);
        inst->untrackDreq();
    }

    // for (Cva6DynInstPtr inst : speculative_queue){
    //     inst->untrackDreq();
    // }
    // speculative_queue.clear();
}

bool
LSUStoreBuffer::canPop(Cva6DynInstPtr inst_){
    if (!speculative_queue.empty() && speculative_queue.front() == inst_){
        DPRINTF(Cva6LSU, "STROREB %s\n", inst_->dreq->name());
    }
    return !speculative_queue.empty() && /* data to commit */
            // speculative_queue.front() == inst_ &&
            // scoreboard.isCommitInst(inst_) && /* Non speculative */
            inst_->dreq->isTranslated() && /* */
            commit_queue.size() < depth_commit; /* Available space*/
}

void
LSUStoreBuffer::pop(Cva6DynInstPtr inst_){
    DPRINTF(Cva6LSU, "mv %s from speculative to commit queue\n", *inst_);
    assert(canPop(inst_));
    assert(speculative_queue.size() > 0);
    assert(commit_queue.size() < depth_commit);
    Cva6DynInstPtr inst = speculative_queue.front();
    assert(inst == inst_);

    speculative_queue.pop_front();
    commit_queue.push(inst);
}

bool
LSUStoreBuffer::advance(){

    // static int pop_lat = 0;
    DPRINTF(Cva6LSU, "Advance... [%d]->[%d]\n", speculative_queue.size(),
        commit_queue.size());

    /** send store_buffer & commit_queue to memory */
    /* Received data ? -> pop */
    if (!commit_queue.empty()){
        Cva6DynInstPtr inst = commit_queue.front();
        DTLBRequestPtr dreq = inst->dreq;
        assert(dreq);
        if (inst->dreq->isCompleted()){
        //     pop_lat++;
        // }
        // if (pop_lat == 3){
        //     pop_lat = 0;
            DPRINTF(Cva6LSU, POP_STR "%s %s\n", *inst, dreq->name());
            inst->untrackDreq();
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
        DPRINTF(Cva6LSU, "Advance... store_buffer->isEmpty()=%d %s %s\n",
        store_buffer->isEmpty(), *amo_buffer, amo_buffer->dreq->name());
    }

    /* Need to send memory request ? */
    if (!amo_buffer->isBubble() && /* Data to transfer */
        store_buffer->isEmpty() && /* All stores have drained */
        scoreboard.isCommitInst(amo_buffer) &&
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

/************************************************************************
 * Load Unit
 ***********************************************************************/

bool
LSULoadUnit::canPush(Cva6DynInstPtr inst_){
    /** Note we must have empty store buffer */
    return loadqueue.size() < 8 &&
        !store_buffer->isPageOffsetMatches(inst_);
}

void
LSULoadUnit::push(Cva6DynInstPtr inst_){
    assert(canPush(inst_));
    /* Push */
    DPRINTF(Cva6LSU, PUSH_STR "%s %s\n", *inst_, inst_->dreq->name());
    loadqueue.push(inst_);
    /* Annotate scheduling */
    inst_->dreq->is_cl_req_inorder = !store_buffer->isClMatch(inst_,
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
        if (!inst->dreq->isTranslated()){
            break;
        }
        /* If inst is uncacheable, wait inst until commit head */
        if (inst->dreq->req->isUncacheable()){
            if (!scoreboard.isCommitInst(inst)){
                 break;
            }
        }
        /* Cache is ready */
        if (!insts_in_memory.canPush(inst)){
            break;
        }
        if (cpu.dcache->isBlocked()){
            break;
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
}

bool
LSUBase::canPop(Cva6DynInstPtr inst){
    if (inst->mc_data.is_taken()) {
        return true;
    } else
    {
        if (inst->dreq->fault != NoFault){
            return true;
        } else {
            return destUnit(inst)->canPop(inst);
        }
    }
}

void
LSUBase::pop(Cva6DynInstPtr inst){
    if (inst->mc_data.is_taken()) {
        assert(inst->dreq);
        inst->dreq->complete_forward(inst->mc_data.value);
        DPRINTF(Cva6LSU, POP_STR "%s %s\n", *inst, inst->dreq->name());
    } else {
        if (inst->dreq->fault != NoFault){
            // Nothing to do
        } else {
            destUnit(inst)->pop(inst);
        }
    }

    if (mc.isEnable()){
        if (inst->dreq->fault == NoFault &&
            inst->staticInst->isLoad() &&   /* Load operation */
            inst->dreq->isBufferable() &&
             /* cacheable, no amo, no llsc, no cmo */
            !inst->mc_data.hit              /* Newer */
        ){
            if (inst->dreq->isCl()){
                mc.writeback_load_speculative(inst->dreq->getClPaddr(),
                    inst->dreq->getClSize(), inst->dreq->getClData(),
                    inst->mc_data);
            }
        }
    }
}

void
LSUBase::flushfrom(Cva6DynInstPtr inst_){
    load_unit.flushfrom(inst_);
    store_unit.flushfrom(inst_);
    mc.flush(); // TODO !!!! Inflight store!
    lsu_fifo.flushfrom(inst_);
}

bool
LSUBase::advance(){

    DPRINTF(Cva6LSU, "Advance ... [%d]\n", lsu_fifo.size());
    for (Cva6DynInstPtr inst: lsu_fifo){
        if (!inst->dreq->isLaunched()){
            inst->dreq->translateTiming();
        }
    }

    while (!lsu_fifo.empty()){
        Cva6DynInstPtr inst = lsu_fifo.front();
        DPRINTF(Cva6LSU, "Select Queue : %s : %s\n", *inst,
            inst->dreq->name());
        if (!inst->dreq->isTranslated()){
            break;
        }

        if (inst->dreq->fault != NoFault){
            lsu_fifo.pop(inst);
            continue;
        } else {
            assert(inst->dreq->req->hasPaddr());
            uint64_t paddr = inst->dreq->req->getPaddr();
            uint64_t size = inst->dreq->req->getSize();

            /** 0) Bypass LQ with minicache */
            if (inst->staticInst->isLoad() &&
               mc.isEnable() &&
               inst->dreq->isBufferable() &&
               // Cacheable , no amo, no llsc, no cmo */
               !scoreboard.is_fence_issued()
            ){
                if (mc.lookup(paddr, size, inst->mc_data)){
                    lsu_fifo.pop(inst);
                    mc.load_issue(paddr, inst->mc_data);
                    continue;
                }
            }

            /** 1) Actual LSU*/
            if (destUnit(inst)->canPush(inst)){
                destUnit(inst)->push(inst);
                lsu_fifo.pop(inst);
                if (mc.isEnable() &&
                    !inst->dreq->req->isUncacheable()){
                    if (inst->dreq->req->getFlags() != 0){
                        mc.clear(paddr);
                    } else if (inst->staticInst->isLoad()){
                        mc.load_issue(paddr, inst->mc_data);
                    } else {
                        mc.write_speculative(paddr, size,
                            inst->dreq->getData(), inst->mc_data);
                    }
                } else {
                }
                continue;
            }
        }
        break; // Break if no access emitted
    }
    store_unit.advance();
    load_unit.advance();
    return true;
}

} // namespace cva6
} // namespace gem5
