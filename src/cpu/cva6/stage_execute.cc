/**
 * @file stage_execute.cc
 * @author Pierre Ravenel (pravenel@kalrayinc.com)
 * @brief
 * @version x
 * @date 2023-05-25
 *
 */

#include "cpu/cva6/stage_execute.hh"

#include <functional>
#include <iomanip>


#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/exec_context.hh"
#include "cpu/cva6/pipeline.hh"
#include "cpu/op_class.hh"
#include "debug/Branch.hh"
#include "debug/Cva6BC.hh"
#include "debug/Cva6Execute.hh"
#include "debug/Cva6Interrupt.hh"
#include "debug/Cva6Mem.hh"
#include "debug/Cva6Minicache.hh"
#include "debug/Cva6Trace.hh"
#include "debug/Cva6X.hh"
#include "debug/Drain.hh"
#include "debug/ExecFaulting.hh"
#include "debug/PCEvent.hh"

namespace gem5 {
namespace cva6 {

bool commit_in_uop = false;

bool
BlockCommit::commitFunctionnal(){
    /* Some Stats */
    stats.committed_block_size.sample(fifo.size());
    stats.committed += fifo.size();
    ArchRegFile<uint64_t> cnt;
    for (Cva6DynInstPtr& inst: fifo){
        if (inst->regs_dst_phy.size()){
            stats.regwrite++;
            stats.regwriteeff += cnt[inst->regs_dst_phy.front()] == 0;
            cnt[inst->regs_dst_phy.front()]++;
        }
    }
    /* Perform commit */
    DPRINTF(Cva6BC, "Block Commit FIFO (#%d)\n", fifo.size());
    while (!fifo.empty()){/* Commit everything */
        Cva6DynInstPtr inst = fifo.pop();
        DPRINTF(Cva6BC, "Commit %s\n", *inst);
        bool need_squash = commitInst(cpu, inst);
        cpu.pipeline->sa.regalloc.commit(inst); /* register release !*/
        if (need_squash){ // Squash must be the last one
            assert(fifo.empty());
            return true;
        }
    }
    return false;
}

bool
BlockCommit::pre_commit(Cva6DynInstPtr& inst){
    DPRINTF(Cva6Execute, "PRECOMMIT %s (#%d : %s)\n",
        *inst, fifo.size(),
        preg_in_flight.dump_match(true));
    /* Push and increment spec commit pointer */
    fifo.push(inst);
    cpu.pipeline->sa.regalloc.pre_commit(inst); /* register release !*/
    cpu.pipeline->iq.pre_commit(inst);

    /* Do this here to avoid shadow miss !!! */
    cpu.pipeline->hcpred.commit(inst);

    if (inst->regs_dst_phy.size()){
        PhysicalReg& reg = inst->regs_dst_phy.front();
        bool inrf = inst->free_reg_at_commit;
        preg_in_flight[reg] = !inrf;
        preg_val[reg] = reg.value;
    }
    /* We can commit everything */
    if (preg_in_flight.isall(false)){ // All are ready to be commited
        return commitFunctionnal();
    }
    return inst->isASquash();
}

void
BlockCommit::clear(){ /* Reset everything */
    DPRINTF(Cva6BC, "*** Block DROP FIFO (#%d) ***\n", fifo.size());
    while (!fifo.empty()){
        Cva6DynInstPtr inst = fifo.pop();
        DPRINTF(Cva6BC, "DROP   %s\n", *inst);
    }
    preg_in_flight.setall(false);
}

void
BlockCommit::dump(){
    if (!(GEM5_UNLIKELY(TRACING_ON && ::gem5::debug::Cva6Execute))) {
        return;
    }
    if (!fifo.size()){
        return;
    }
    DPRINTF(Cva6Execute, "BlockCommit FIFO (#%d : %s)\n",
        fifo.size(),  preg_in_flight.dump_match(true));
    for (Cva6DynInstPtr inst: fifo){
        DPRINTF(Cva6Execute, "%s\n", *inst);
    }
}

BranchData getEffectiveBranch(Cva6DynInstPtr inst){
    if (!inst->isFault() && inst->staticInst->isCondCtrl()){
        if (inst->isTaken() != inst->pc_next_taken){
            fatal("Badly measured taken for %s\n", *inst);
        }
    }
    return BranchData(
        inst->triedToPredict,
        inst->isASquash(),
        inst->id.fetchSeqNum,
        *inst->pc_next,
        inst->pc_next_taken);
}

void
Execute::tryToBranch(Cva6DynInstPtr inst, BranchData &branch){
    branch = getEffectiveBranch(inst);
    // Squash at latest valid pc
    branch.setSquashTarget(cpu.getContext()->pcState());

    DPRINTF(Branch, "tryToBranch : %s\n", branch);
}



/** Do the stats handling and instruction count and PC event events
 *  related to the new instruction/op counts */
void doInstCommitAccounting(Cva6CPU& cpu, Cva6DynInstPtr inst){
    if (inst->isFault()){ // Skip fault
        return;
    }
    Cva6Thread *thread = cpu.thread;
    /* Increment the many and various inst and op counts in the
     *  thread and system */
    if (!inst->staticInst->isMicroop() || inst->staticInst->isLastMicroop())
    {
        thread->numInst++;
        thread->threadStats.numInsts++;
        cpu.stats.numInsts++;
        /* Act on events related to instruction counts */
        thread->comInstEventQueue.serviceEvents(thread->numInst);
    }
    thread->numOp++;
    thread->threadStats.numOps++;
    cpu.stats.numOps++;
    cpu.probeInstCommit(inst->staticInst, inst->pc->instAddr());
}

bool commitInst(Cva6CPU& cpu, Cva6DynInstPtr inst){
    // ThreadContext *thread = cpu.thread->getTC(); // cpu.getContext();

    inst->executeCommit(cpu, *cpu.thread);
    doInstCommitAccounting(cpu, inst);

    /* Update state */
    commit_in_uop = !inst->isFault() &&
                    inst->staticInst->isMicroop() &&
                    !inst->staticInst->isLastMicroop();

    cpu.pipeline->iq.commit(inst); /* Post-commit (for stores SQS->SQC)!*/
    cpu.pipeline->plugins.commit(inst);
    cpu.pipeline->sa.regalloc.post_commit(inst); /* Register in ARF */

    /* Update BP */
    BranchData branch = getEffectiveBranch(inst);
    if (branch.need_squash){
        if (branch.is_predicted) { // Mret ...
            // TODO guard !
            // assert(inst->staticInst->isNonSpeculative());
            cpu.pipeline->bp.squash(branch.num,
                *branch.target, branch.actually_taken, 0);
        } else {
            cpu.pipeline->bp.squash(branch.num, 0);
        }
    }
    if (branch.is_predicted) {
        cpu.pipeline->bp.update(branch.num, 0);
    }

    return branch.need_squash;
}

void
Execute::evaluate() {
    /* Check interrupts first */
    if (checkInterrupts()){ // TODO !
        /* Signalling an interrupt this cycle */
        Fault interrupt = cpu.getInterruptController()->getInterrupt();
        assert(interrupt != NoFault);
        /* The interrupt *must* set pcState */
        cpu.getInterruptController()->updateIntrInfo();
        interrupt->invoke(cpu.getContext());
        DPRINTF(Cva6Interrupt, "Invoking interrupt: %s\n",
            interrupt->name());
        cpu.pipeline->sa.fixer.clear_on_it();
        stats.flush += 1;
        stats.flush_it += 1;
        stats.flush_it_drop += cpu.pipeline->bc.size();;
        do_flush();
        return;
    }

    /* Execute stage */
    while (inp.canPop()){
        inp.pop(); // Instruction are already pushed in fus
    }

    /* Execute WB : Process result  */
    cpu.pipeline->fus.advance();
    for (Cva6DynInstPtr inst: cpu.pipeline->iq.getIssueQueue()){
        // For all instructions in FUs try to complete the execution
        if (!inst->isInFu()){
            continue;
        }
        if (!cpu.pipeline->fus.canPop(inst)){
            continue;
        }
        cpu.pipeline->fus.pop(inst);        /* Compute FU and pop */
        inst->executeComplete();            /* Complete FU result */
        cpu.pipeline->iq.completeInst(inst);  /* Notify scoreboard */
    }

    // Test branch prediction at execute
    if (flushAtExecute){
        for (Cva6DynInstPtr inst: cpu.pipeline->rob){
            if (inst->execute_completed &&
            inst->isMisspredict() &&
            !inst->staticInst->isNonSpeculative()){
                // Squash to the target
                inst->_no_equal_when_match = true;
                // Hack to preserve inst
                BranchData branch = getEffectiveBranch(inst);
                cpu.pipeline->flushfrom(inst, branch);
                inst->_no_equal_when_match = false;
                // Fix inst and pred target and squash
                inst->fixBranchPrediction();
                break; // Stop
            }
        }
    }

    /* Commit stage */
    for (int i = 0; i < commitWidth; i++){ // Dual port commit
        DPRINTF(Cva6Execute, "Attempting to retire (port%d)\n", i);
        /* Get the Valid Issue Unit */
        if (!cpu.pipeline->rob.size()) {
            break; // No instruction to commit
        }

        Cva6DynInstPtr inst = cpu.pipeline->rob.front();
        DPRINTF(Cva6Execute, "rob entry: %s\n", *inst);

        if (!inst->execute_completed){
            DPRINTF(Cva6Execute, "(port%d) inst is not ex : %s\n", i, *inst);
            break;
        }

        uint64_t pcstore;
        bool is_mem_violation = cpu.pipeline->mdpc.isViolation(inst, pcstore);
        /* Check is mdpc is valid */
        /* Care inst must have a valid paddr */
        // if (is_mem_violation != !inst->break_memory_order->isBubble()){
        //     fatal("REF: %s\n VS testid: %d / pc %lx\n",
        //      *inst->break_memory_order, inst->last_store_id,
        //         inst->last_store_pc);
        // }

        /* Inst produced bad value */
        if (is_mem_violation){
            DPRINTF(Cva6Execute, "MISSPRED MEM ORDER : %s\n", *inst);
            uint64_t pcself = inst->pc->instAddr();
            DPRINTF(Cva6Execute, "Mark violation %lx -> %lx\n",
                    pcstore, pcself);
            if (inst->staticInst->isLoad()){
                cpu.pipeline->mdp.violation(pcstore, pcself);
            } else {
                assert(0); // Cannot go here
                cpu.pipeline->mdp.violation(pcself, pcstore);
                cpu.pipeline->mdp.violation(pcstore, pcself);
            }
            stats.flush ++;
            stats.flush_mdp += 1;
            stats.flush_mdp_drop += cpu.pipeline->bc.size();
            cpu.pipeline->hcpred.violation(inst);
            // assert(0);
            do_flush();
            return; /* EARLY FLUSH : do not commit */
        }
        if (!inst->isFault() && inst->staticInst->isLoad()){
            cpu.pipeline->hcpred.confidence(inst);
        }

        /* Compare the pred base addr with the real one */
        bool misspred_addr = cpu.pipeline->dpe.post_commit(inst);
        if (misspred_addr){
            DPRINTF(Cva6Execute, "MISSPRED ADDR : %s\n", *inst);
            stats.flush ++;
            stats.flush_vp += 1;
            do_flush();
            return; /* EARLY FLUSH : do not commit */
        }

        /* Pre commit (that can be reversed)*/
        assert(cpu.pipeline->rob.front() == inst);
        cpu.pipeline->rob.pop(inst); /* Pre-commit */
        cpu.pipeline->mdpc.commit(inst); /* must be pre-commit ?? */
        /* Fault have to be detected before enter BC ? */
        cpu.pipeline->sa.pre_commit(inst); /* Annotate can commit */

        /* oracle to mark MDP missprediction */
        // cpu.pipeline->iq.markMemoryViolation(inst);

        // bool is_serialise = !inst->isFault() && needSerial
        //     inst->isLastOpInInst() &&
        //     (inst->staticInst->isSerializeAfter() ||
        //     inst->staticInst->isSquashAfter());

        if (inst->needSerialise) { /* Inst was serialised in the scheduler*/
            // Check scheduler serialisation
            // Everything must be comitted
            assert(cpu.pipeline->bc.empty());
        }

        /* Try to commit */
        bool need_squash = cpu.pipeline->bc.pre_commit(inst); /* Ex CSRW ! */
        /* Some stats */
        if (inst->isASquash()){
            stats.flush ++;
            if (!inst->isFault()
                && inst->staticInst->isSerializeAfter()){
                stats.flush_serialise += 1;
                stats.flush_serialise_drop += cpu.pipeline->bc.size();
            }

            stats.flush_squashafter += !inst->isFault()
                && !inst->staticInst->isSerializeAfter()
                && inst->staticInst->isSquashAfter();
            stats.flush_fault += inst->isFault();
            stats.flush_cond_direct += !inst->isFault()
                && inst->staticInst->isCondCtrl()
                && inst->staticInst->isDirectCtrl();
            stats.flush_cond_indirect += !inst->isFault()
                && inst->staticInst->isCondCtrl()
                && inst->staticInst->isIndirectCtrl();
            stats.flush_uncond_direct += !inst->isFault()
                && inst->staticInst->isUncondCtrl()
                && inst->staticInst->isDirectCtrl();
            stats.flush_uncond_indirect += !inst->isFault()
                && inst->staticInst->isUncondCtrl()
                && inst->staticInst->isIndirectCtrl();

            if (inst->isFault()
                 && inst->staticInst
                 && inst->staticInst->isLoad()) {
                stats.flush_load += 1;
                stats.flush_load_drop += cpu.pipeline->bc.size();
            }

            // Account the dropped instructions
            if (!inst->isFault() && inst->staticInst->isControl()) {
                stats.flush_control += 1;
                stats.flush_control_drop += cpu.pipeline->bc.size();
            }
        }

        // Oracle: Early commit
        if (oracleEarlyCommit){
            cpu.pipeline->bc.commitFunctionnal();
        }

        if (need_squash){ /* Handle flush */
            if (0){ // Oracle: Late commit and flush in place for nothing
                cpu.pipeline->bc.commitFunctionnal();
            }
            if (!cpu.pipeline->bc.empty()){
                cpu.pipeline->sa.fixer.apply_fix_for(inst,
                    true, true, cpu.pipeline->bc.size());
            }

            // // We can't delay the fault as we reach stale point
            // // Annotate the scheduler to RR barrier this fault
            // // Also forward a valid prediction
            // if (!inst->isFault() && inst->staticInst->isControl() &&
            //     inst->staticInst->isDirectCtrl()){ // Fix BP
            //     bool actually_taken = inst->pc_next_taken;
            //     std::unique_ptr<PCStateBase> &target = inst->pc_next;
            //     // for (int i = 0; i < 4; i++){
            //     //     cpu.pipeline->bp.update_table_only(
            //     //         inst->id.fetchSeqNum, 0,
            //     //         inst->pc_next_taken, *inst->pc_next);
            //     // }
            //     /* Force pc next, no barrier */
            //     // PR: CARE w/o BARRIER : tryToBranch drop hists!
            // } else {
            //     /* Force pc next, barrier */
            //     cpu.pipeline->sa.fixer.apply_fix_for(inst, true, true);
            // }

            /* Fix Ipred early as there is no conf in Ipred */
            if (!inst->isFault() &&
                inst->staticInst->isUncondCtrl() &&
                inst->staticInst->isIndirectCtrl() &&
                !inst->staticInst->isReturn()){
                BranchData branch = getEffectiveBranch(inst);
                cpu.pipeline->bp.squash(branch.num,
                    *branch.target, branch.actually_taken, 0);
            }
            do_flush();
            return;
        } /* Otherwise take the valid path */

        // We can pre-commit the instruction
        // commitInst(inst); /* Post-commit */
        // Already done is commitInst !
        // if (need_squash){ // And not handled by bc
        //     tryToBranch(inst, resolved_branch); /* POST-commit */
        // }

        bool misspred_value = cpu.pipeline->dpe.commit(inst);
        if (misspred_value){
            do_flush();
            return;
        }
    }
    cpu.pipeline->bc.dump();
}


void
Execute::do_flush(){
    // Flush everything and start at the latest uarch state
    BranchData inplace = BranchData::SquashAt(cpu);
    cpu.pipeline->flushfrom(Cva6DynInst::bubble(), inplace);
}

void
Execute::flushfrom(Cva6DynInstPtr inst){
    DPRINTF(Cva6Execute, "Flush fus & inp\n");
    cpu.pipeline->fus.flushfrom(inst);
    inp.flushfrom(inst);
}

bool
Execute::checkInterrupts(){
    assert(FullSystem && cpu.getInterruptController());
    // TODO lastCommitWasEndOfMacroop
    if (cpu.checkInterrupts()) {
        DPRINTF(Cva6Execute, "CAN IT?\n");
        if (commit_in_uop){
            return false;
        }
        /* lsu chech in memory instruction ! */
        return cpu.pipeline->iq.canInterrupts() &&
               cpu.pipeline->bc.canInterrupts();
    }
    return false;
}


} // namespace cva6
} // namespace gem5
