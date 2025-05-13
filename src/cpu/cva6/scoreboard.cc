/**
 * @file scoreboard.cc
 * @author Pierre Ravenel (pravenel@kalrayinc.com)
 * @brief A simple instruction scoreboard for tracking dependencies
 * @version 1.0
 * @date 2023-05-25
 */

#include "cpu/cva6/scoreboard.hh"
#include "cpu/cva6/pipeline.hh"
#include "cpu/reg_class.hh"
#include "debug/Cva6Scoreboard.hh"

namespace gem5 {
namespace cva6 {

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
Scoreboard::getRegState(Cva6DynInstPtr inst_in, PhysicalReg& reg,
    Cva6DynInstPtr &producer){
    if (reg.valid){
        return true;
    }
    switch(sb[reg]){
        case FREE: {
            /* Read commited value */
            reg.fromrf = true;
            if (_sb_is_unsafe[reg]){ /* FIX IT*/
                reg.set(cpu.pipeline->bc.preg_val[reg]);
                reg.fromrf_unsafe = true;
                DPRINTF(Cva6Scoreboard, "Read %s:%lx from RFU!\n", reg,
                    reg.value);
            } else {
                reg.set(cpu.thread->getReg(reg.regid));
                DPRINTF(Cva6Scoreboard, "Read %s:%lx from RF\n", reg,
                    reg.value);
            }
            break;
        }
        case IN_USE: {
            DPRINTF(Cva6Scoreboard, "Read %s IN_USE\n", reg);
            producer = _sb_producer[reg];
            /* We have to wait */
            break;
        }
        case FWABLE: {
            reg.set(prf[reg]);
            DPRINTF(Cva6Scoreboard, "Read %s:%lx from FW\n", reg, reg.value);
            if (prf_isfault[reg]){ /* Forward fault */
                Fault fault = NeverCommitFault::fault();
                inst_in->setFaultEx(fault);
            }
            break;
        }
    }
    return reg.valid;
}

bool
Scoreboard::chechIssueInst(
    Cva6DynInstPtr inst,
    bool &is_raw,
    Cva6DynInstPtr &producer,
    bool &is_waw){

    is_raw = false;
    is_waw = false;
    if (inst->isBubble()){
        return false;
    }

    /* Available source registers */
    // RaW dependencies
    for (PhysicalReg &reg: inst->regs_src_phy){
        is_raw |= !getRegState(inst, reg, producer);
    }
    /* Fault does not have register dependancies */
    if (inst->isFault()){
        is_raw = false;
    }
    if (inst->vp_data.addr_taken){ // Bypass IRO
        assert(inst->staticInst->isLoad());
        inst->regs_src_phy[0].set(inst->vp_data.t1_addr);
        is_raw = false;
    }

    /* Available destination registers */
    // WaW dependencies
    for (PhysicalReg &reg: inst->regs_dst_phy){
        is_waw |= sb[reg] == IN_USE; // Cannot be true in full RR
    }
    // WaR dependencies
    // Nothing to do
    // Solved at pre-scheduling

    // RaR dependencies
    // Nothing to do
    return !is_raw && !is_waw;
}
Cva6DynInstPtr
Scoreboard::getIssueInst(
    bool &is_over_serialise,
    bool &is_raw,
    Cva6DynInstPtr &producer,
    bool &is_waw
){
    Cva6DynInstPtr inst = Cva6DynInst::bubble();
    is_over_serialise = false;
    is_raw = false;
    is_waw = false;
    /* First check serialisation */
    is_over_serialise = is_serialise_inflight;
    /* Second, retrieve instruction from previous stage */
    if (!inp.canPop()){
        return Cva6DynInst::bubble();
    }
    inst = inp.front();
    chechIssueInst(inst, is_raw, producer, is_waw);
    return inst;
}

void
Scoreboard::onInsert(Cva6DynInstPtr inst){
    assert(!inst->stage_issue_enter);
    inst->stage_issue_enter = true;
    /* Markup serialisation */
    is_serialise_inflight += inst->needArchSerialize;
    /* Markup registers */
    for (PhysicalReg &reg: inst->regs_dst_phy){
        if (prf_isvp[reg]){ /* Nothing to do */
            continue;
        }
        // fatal_if(sb[reg] != FREE, "Reg %s must be freed\n", reg);
        // v2 with waw overlap
        fatal_if(sb[reg] == IN_USE, "Reg %s must not be pending\n", reg);
        sb[reg] = IN_USE;
        _sb_producer[reg] = inst;
    }
}

void
Scoreboard::completeIssueInst(Cva6DynInstPtr inst){
    assert(inst->stage_issue_enter);
    assert(!inst->issue_completed);
    inst->issue_completed = true;

    /* Some stats */
    for (PhysicalReg &reg: inst->regs_src_phy){
        stats.reg_read += 1;
        stats.reg_read_fw += !reg.fromrf;
        stats.reg_read_commit += reg.fromrf;
        stats.reg_read_commit_unsafe += reg.fromrf_unsafe;
    }

    /* Finally insert in the issue_queue (debug) */
    // assert(issue_queue.size() < nr_entries);
    // The queue cannot overflow as we use PRF
    issue_queue.push_back(inst);
}

void
Scoreboard::issueInst(Cva6DynInstPtr inst){
    Cva6DynInstPtr i2 = inp.pop();
    fatal_if(i2 != inst, "Sched inst must be this one\n");
    onInsert(inst); /* Markup rd buzy */
    completeIssueInst(inst); /* (debug) and stats */
}

void
Scoreboard::writeBackRF(Cva6DynInstPtr& inst){
    assert(inst->execute_completed);
    for (PhysicalReg &reg: inst->regs_dst_phy){
        if (prf_isvp[reg]){ /* Clear vp flags because no more vp */
            assert(sb[reg] == FWABLE);
            prf_isvp[reg] = false;
        } else {
            assert(sb[reg] == IN_USE); // We must expect a WB
            sb[reg] = FWABLE;
        }

        /* Set value and fault */
        if (inst->isFault()){
            prf_isfault[reg] = true;
            prf[reg] = 0x12345678deaddead; /* debug only */
        } else {
            assert(reg.valid);
            prf[reg] = reg.value;
        }
        /* Some stats */
        stats.reg_write_fw += 1;
    }
}

void
Scoreboard::completeInst(Cva6DynInstPtr inst) {
    assert(!inst->execute_completed); // not already commplete
    inst->execute_completed = true; // Finished execution
    if (!inst->isFault() && inst->staticInst->isNonSpeculative()){
        return;
        // Bypass the WB as the instruction is pending
    }
    /* Otherwise write back */
    writeBackRF(inst);
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
Scoreboard::pre_commit(Cva6DynInstPtr& inst){
    for (auto& reg: inst->regs_dst_phy){
        _sb_is_unsafe[reg] = true;
    }
}

void
Scoreboard::commitInst(Cva6DynInstPtr& inst){
    assert(!inst->commit_completed); // Already commited
    inst->commit_completed = true;
    for (auto& reg: inst->regs_dst_phy){
        _sb_is_unsafe[reg] = false;
    }
    if (!inst->isFault() && inst->staticInst->isNonSpeculative()){
        /* Do the write-back now */
        writeBackRF(inst);
    }
    /* There is no need to free the register !! */
    /* Free registers  */
    // for (PhysicalReg &reg: inst->regs_dst_phy){
    //     assert(sb[reg] == FWABLE);
    //     sb[reg] = FREE;
    // }
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
        is_serialise_inflight -= inst->needArchSerialize;
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
    prf_isvp.setall(false);
    is_serialise_inflight = 0;
    _sb_is_unsafe.setall(false);
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
        if (i2->id.fetchSeqNum == inst->id.fetchSeqNum){
            /* FIRST OF ALL:  Stop at ourself !*/
            break;
        }
        if (i2->commit_completed){ /* Skip committed */
            continue;
        }
        assert(i2->issue_completed); // InO issue
        /* Is a load */
        if (i2->isFault() || !i2->staticInst->isLoad()){
            continue;
        }
        assert(i2->dreq);
        if (!i2->dreq->req->hasPaddr()){ /* The load must be translated */
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
    if (!(GEM5_UNLIKELY(TRACING_ON && ::gem5::debug::Cva6Scoreboard))) {
        return;
    }
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
            DPRINTF(Cva6Scoreboard, "sbe#%3d [%c][%c][%c] %s%s\n",
            i++, hit[inst->issue_completed], hit[inst->execute_completed],
            hit[inst->commit_completed], os.str(), *inst);
        }
    }
}

bool ScoreboardO3::isReady(Cva6DynInstPtr& inst, bool &is_raw,
    Cva6DynInstPtr &producer, bool &is_waw, bool &is_ss){
    // bool deps_ready =
    chechIssueInst(inst, is_raw, producer, is_waw);
    is_waw = false; // There is no WaW in OoO

    /* Functionnal wire from FUs */
    bool fu_ready = cpu.pipeline->fus.canPush(inst);

    is_ss = !inst->isFault() &&
            inst->staticInst->isStore() &&
            inst != store_order.front();

    /* TODO: MDP*/
    return !is_raw && fu_ready && !is_ss;
}

} // namespace cva6
} // namespace gem5
