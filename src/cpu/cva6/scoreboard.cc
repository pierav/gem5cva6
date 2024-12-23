/**
 * @file sb.hh
 * @author Pierre Ravenel (pravenel@kalrayinc.com)
 * @brief
 * @version 0.1
 * @date 2023-05-25
 */

#include "cpu/cva6/scoreboard.hh"
#include "cpu/cva6/pipeline.hh"
#include "cpu/reg_class.hh"
#include "debug/Cva6Scoreboard.hh"

namespace gem5 {
namespace cva6 {

bool
Scoreboard::canPush(){
    return issue_queue.size() < nr_entries;
}

void
Scoreboard::pushInst(Cva6DynInstPtr inst){
    assert(issue_queue.size() < nr_entries);
    issue_queue.push_back(inst);
}

bool
Scoreboard::isUnissedStoreBefore(Cva6DynInstPtr inst_in){
    int pos = issue_queue.size(); // Default is outside sb
    // Find instruction position in sb
    for (int i = 0; i < issue_queue.size(); i++){
        assert(!issue_queue[i]->isBubble());
        if (issue_queue[i]->isAfterOrEqual(inst_in)){
            #ifdef NO_LAMBDA_PROBE
            assert(issue_queue[i] == inst_in);
            #endif
            pos = i;
            break;
        }
    }

    for (int i = pos - 1; i >= 0; i--){
        Cva6DynInstPtr inst = issue_queue[i];
        if (inst->isFault()){ // Stall after fault
            return true;
        }
        StaticInstPtr si = inst->staticInst;
        if (si->isStore() || si->isAtomic()){
            if (!inst->issue_completed){ /* Cannot match addr in LSU */
                return true;
            }
            /* Otherwise, let the lsu decide */
        }
    }
    return false;
}

bool
Scoreboard::getRegState(Cva6DynInstPtr inst_in, PhysicalReg& reg){
    switch(sb[reg]){
        case FREE: {
            /* Read commited value */
            reg.set(cpu.thread->getReg(reg.regid));
            reg.fromrf = true;
            break;
        }
        case IN_USE: {
            /* We have to wait */
            break;
        }
        case FWABLE: {
            reg.set(prf[reg]);
            if (prf_isfault[reg]){ /* Forward fault */
                Fault fault = NeverCommitFault::fault();
                inst_in->setFaultEx(fault);
            }
            break;
        }
    }
    return reg.valid;
    #if 0
    int pos = issue_queue.size(); // Default is outside sb
    // Find instruction position in sb
    // The Order is now not valid
    // for (int i = 0; i < issue_queue.size(); i++){
    //     assert(!issue_queue[i]->isBubble());
    //     if (issue_queue[i]->isAfterOrEqual(inst_in)){
    //         pos = i;
    //         break;
    //     }
    // }
    // dump();
    // fatal_if(pos == -1, "Instruction %s not in sb\n", *inst_in);

    /*
     * sbe0: addi x5, x0, 1 <-- commit head
     * sbe1: addi x0, x5, 1
     * sbe2: addi x5, x5, 1                 // depends on sbe0
     * sbe3: addi x5, x5, 1 <-- Issue head  // depends on sbe2
     */
    // From oldest to newest try to find register
    for (int i = pos - 1; i >= 0; i--){
        Cva6DynInstPtr inst = issue_queue[i];
        // No more true !
        // if (inst->isFault()){ // Stall after fault
        //     return false;
        // }
        StaticInstPtr si = inst->staticInst;
        for (PhysicalReg& ireg: inst->regs_dst_phy){
            if (reg == ireg){
                reg.value = ireg.value;
                reg.valid = ireg.valid;
                return reg.valid;
            }
        }
    }
    /* No forwarding, read RF */
    reg.set(cpu.thread->getReg(reg.regid));
    reg.fromrf = true;
    return reg.valid;
    #endif
}

bool
Scoreboard::canInstIssue(Cva6DynInstPtr inst) {

    /* Fault does not have register dependancies */
    if (inst->isFault())
        return true;

    /* Available source registers */
    // RaW dependencies
    int ok = 1;
    for (PhysicalReg &reg: inst->regs_src_phy){
        ok &= getRegState(inst, reg);
    }
    if (!ok){ return false; }

    /* Available destination registers */
    // WaW dependencies

    // WaR dependencies
    // Nothing to do

    // RaR dependencies
    // Nothing to do

    return true;
}

Cva6DynInstPtr
Scoreboard::getIssueInst(
    size_t index,
    bool &is_over_serialise,
    bool &is_ready
){
    Cva6DynInstPtr inst = Cva6DynInst::bubble();
    is_over_serialise = false;
    is_ready = false;
    if (!canPush()){
        return Cva6DynInst::bubble();
    }
    /* First check serialisation */
    // for (Cva6DynInstPtr dyn: issue_queue){
    //     if (inst->needSerialise){
    //         is_over_serialise = true;
    //         break;
    //     }
    // }
    is_over_serialise = is_serialise_inflight;
    /* Second, retrieve instruction from previous stage */
    if (!cpu.pipeline->sa.can_pop_scheduled()){
        return Cva6DynInst::bubble();
    }
    inst = cpu.pipeline->sa.front_scheduler();
    is_ready = canInstIssue(inst);
    return inst;
}

void
Scoreboard::issueInst(Cva6DynInstPtr inst){
    assert(!inst->issue_completed);
    inst->issue_completed = true;
    Cva6DynInstPtr i2 = cpu.pipeline->sa.pop_scheduler();
    fatal_if(i2 != inst, "Sched inst must be this one\n");
    /* Markup serialisation */
    is_serialise_inflight += inst->needSerialise;
    /* Markup registers */
    for (PhysicalReg &reg: inst->regs_dst_phy){
        fatal_if(sb[reg] != FREE, "Reg %s must be freed\n", reg);
        sb[reg] = IN_USE;
    }
}

void
Scoreboard::completeInst(Cva6DynInstPtr inst) {
    assert(!inst->execute_completed); // not already commplete
    inst->execute_completed = true; // Finished execution
    for (PhysicalReg &reg: inst->regs_dst_phy){
        assert(sb[reg] == IN_USE);
        sb[reg] = FWABLE;
        prf[reg] = reg.value;
        if (inst->isFault()){
            prf_isfault[reg] = true;
        }
    }
}

Cva6DynInstPtr
Scoreboard::getCommitInst(size_t index){
    if (index >= issue_queue.size()){
        // No more instruction to commit 1 >= 1
        DPRINTF(Cva6Scoreboard, "commit stall: no instruction\n");
        return Cva6DynInst::bubble();
    }

    Cva6DynInstPtr inst = issue_queue[index];
    return inst;
}

void
Scoreboard::commitInst(Cva6DynInstPtr inst) {
    assert(!inst->commit_completed); // Already commited
    inst->commit_completed = true;
    /* Free registers  */
    for (PhysicalReg &reg: inst->regs_dst_phy){
        assert(sb[reg] == FWABLE);
        sb[reg] = FREE;
    }
}

void
Scoreboard::tick(){
    /* For all instructions to commit */
    while (!issue_queue.empty() &&
          issue_queue.front()->commit_completed) {
        /* Remove instruction from sb*/
        /* pop issue queue*/
        Cva6DynInstPtr inst = issue_queue.front();
        issue_queue.pop_front();
        is_serialise_inflight -= inst->needSerialise;
    }
}

void
Scoreboard::flush(){
    DPRINTF(Cva6Scoreboard, "flush\n");
    // Flush issue queue
    issue_queue.clear();
    /* Clear inflights registers */
    sb.setall(FREE);
    prf_isfault.setall(false);
    is_serialise_inflight = 0;
}

#if 0
Cva6DynInstPtr
Scoreboard::flush_value_from(Cva6DynInstPtr inst_error, bool force){
    DPRINTF(Cva6Scoreboard, "flush_value\n");
    // Unissue all instructions excepts insts that are out of FUS
    bool do_flush = false;

    assert(!inst_error->isBubble());
    // assert(!inst_error->isFault());
    assert(inst_error->staticInst);
    assert(inst_error->staticInst->isLoad());
    assert(inst_error->staticInst->numDestRegs() == 1);
    RegId reg_error = inst_error->staticInst->destRegIdx(0);

    /* Find instruction error position */
    size_t inst_index = -1;
    for (int i = 0; i < issue_queue.size(); i++){
        if (issue_queue[i] == inst_error){
            inst_index = i;
            break;
        }
    }
    fatal_if(inst_index == -1, "instruction not in sb\n");

    // #if 0
    /** Check if there is a reg dependancy */
    bool is_reg_dep = force;
    for (int i = inst_index+1; i < issue_queue.size(); i++){
        Cva6DynInstPtr inst = issue_queue[i];
        assert(!inst->isBubble());
        if (!inst->issue_completed){
            break;
        }

        if (inst->isFault()){
            is_reg_dep = true;
            break;
        }

        /** Check reg dep (care isFault) */
        for (int j = 0; j < inst->staticInst->numSrcRegs(); j++){
            if (inst->staticInst->srcRegIdx(j) == reg_error){
                // RAW dependancy
                is_reg_dep = true;
                break;
            }
        }

    }

    if (!is_reg_dep){
        // Nothing to do
        return Cva6DynInst::bubble();
    }
    // #endif

    Cva6DynInstPtr vilain = Cva6DynInst::bubble();

    // std::set<RegId> deps = { reg_error };

    /** Begin the flush */
    // PR: -*- dirty flush all
    do_flush = true;
    vilain = issue_queue[0];
    int off = force ? 0 : 1;
    // PR: -*- end config
    for (int i = inst_index+off; i < issue_queue.size(); i++){
        assert(i >= 0);
        // Cva6DynInstPtr previous_inst = issue_queue[i-1];
        Cva6DynInstPtr inst = issue_queue[i];
        assert(!inst->isBubble());

        /** Good termination */
        // No more valid !
        // if (!inst->issue_completed){
        //     break;
        // }
        bool flush_one = false;
        if (!do_flush){

            /** 0) Flush from the first inst in FU */
            // if (!inst->execute_completed){
            //     do_flush = true;
            //     vilain = previous_inst;
            // }

            /** 0.1) */
            if (inst->isFault()){
                do_flush = true;
                vilain = inst;
            } else {
                /** 1) Flush from the first inst with reg deps */
                bool has_reg_dep = false;
                for (int j = 0; j < inst->staticInst->numSrcRegs(); j++){
                    // if (deps.count(inst->staticInst->srcRegIdx(j))){
                    //     // RAW dependancy
                    //     has_reg_dep = true;
                    //     break;
                    // }
                    if (inst->staticInst->srcRegIdx(j) == reg_error){
                        has_reg_dep = true;
                    }
                }

                if (has_reg_dep){
                //     if (inst->execute_completed){
                //         flush_one = true;
                //         if (inst->staticInst->isStore() ||
                // inst->staticInst->isAtomic()){
                //             // We cannot check memory deps
                //             vilain = inst;
                //             do_flush = true;
                //         }
                //     } else {
                //         vilain = inst;
                //         do_flush = true;
                //     }

                    do_flush = true;
                    vilain = inst;
                    // if (inst->staticInst->isDirectCtrl()){
                    //     // No need to begin the flush.
                    //     // Justre reset IT !
                    //     // If the input was badly predicted, the pipeline
                    //     // will be flushed
                    //     assert(inst->staticInst->numDestRegs() == 0);
                    //     flush_one = true;
                    // }else{
                    //     do_flush = true;
                    //     vilain = inst;
                    // }
                }
                /** 2) Flush from the first memory access */
                if (inst->staticInst->isStore()){
                    do_flush = true;
                    vilain = inst;
                }
            }
        }

        /** Flush if needed */
        if (inst->issue_completed){

            // std::ostringstream ossrc;
            // for (int j = 0; j < inst->staticInst->numSrcRegs(); j++){
            //     ossrc << inst->staticInst->srcRegIdx(j) << " ";
            // }
            // std::ostringstream os;
            // // for (RegId id: deps){
            //     os << id << " ";
            // }
            if (do_flush || flush_one){
                DPRINTF(Cva6Scoreboard, "flush : reset %s\n", *inst);
                inst->reset();
            } else {
                DPRINTF(Cva6Scoreboard, "flush :  skip %s\n", *inst);
            }
        } else {
            DPRINTF(Cva6Scoreboard, "flush :  none %s\n", *inst);
        }
    }

    return vilain;
}

#endif

bool
Scoreboard::markMemoryViolation(Cva6DynInstPtr inst){
    /*
     * i1: Store \/ i2: Load
     * i2: Load  /\ i1: Store
     */
    if (inst->isFault() || !inst->staticInst->isStore()){
        return false;
    }
    assert(inst->dreq);
    uint64_t store_addr = inst->dreq->getDWPaddr();
    DPRINTF(Cva6Scoreboard, "STORE MEM CHECK %s\n", *inst);
    bool ret = false;
    for (Cva6DynInstPtr i2: issue_queue){
        /* Check all loads issued before the store */
        DPRINTF(Cva6Scoreboard, "________ CHECK %s\n", *i2);
        if (i2->commit_completed){ // Skip committed
            continue;
        }
        assert(i2->issue_completed); // InO issue
        if (i2 == inst){ //
            break;
        }
        /* Is a load */
        if (i2->isFault() || !i2->staticInst->isLoad()){
            continue;
        }
        /* The load must be translated */
        assert(i2->dreq);
        if (!i2->dreq->req->hasPaddr()){
            continue;
        }
        // /* Is before the store in programme order */
        // if (i2->isAfterOrEqual(inst)){
        //     fatal("Bad schedule!\n");
        //     continue;
        // }
        /* Is address matches */
        uint64_t load_addr = i2->dreq->getDWPaddr();
        DPRINTF(Cva6Scoreboard, "Try addr : %d : %s\n",
            store_addr, load_addr);
        if (store_addr != load_addr){
            continue;
        }
        DPRINTF(Cva6Scoreboard, "BAD DEPS with %s\n", *i2);
        /* Finnaly we detected a memory hazard : mark the load as invalid */
        i2->break_memory_order = inst;
        ret = true;
    }
    return ret;
}

void
Scoreboard::dump(){
    int i = 0;
    char hit[2] = {' ', 'x'};
    DPRINTF(Cva6Scoreboard, "Scoreboard [I][E][C]\n");
    for (Cva6DynInstPtr inst: issue_queue){
        if (!inst->isBubble()){
            std::ostringstream os;
            for (int i = 0; i < 5; i++){
                if ((inst->bb_idx % 5) == i){
                    os << "" << (inst->bb_idx % 100) << " ";
                } else {
                    os << " . ";
                }
            }
            DPRINTF(Cva6Scoreboard, "sbe#%d [%c][%c][%c] %s%s\n",
            i++, hit[inst->issue_completed], hit[inst->execute_completed],
            hit[inst->commit_completed], os.str(), *inst);
        }
    }
}

} // namespace cva6
} // namespace gem5
