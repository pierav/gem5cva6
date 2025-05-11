
#include "cpu/cva6/stage_execute.hh"

#include <functional>
#include <iomanip>

#include "arch/generic/isa.hh"
#include "arch/riscv/pcstate.hh"
#include "arch/riscv/regs/misc.hh"
#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/exec_context.hh"
#include "cpu/op_class.hh"
#include "debug/Branch.hh"
#include "debug/Cva6BC.hh"
#include "debug/Cva6Commit.hh"
#include "debug/Cva6CommitCpt.hh"
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
        // cpu.pipeline->sa.commit(inst); /* Post-commit : register release !*/
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
    cpu.pipeline->sa.commit(inst); /* Post-commit : register release !*/
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

    DPRINTF(Branch, "tryToBranch : %s\n", branch.dump());
}

/** Do the stats handling and instruction count and PC event events
 *  related to the new instruction/op counts */
void doInstCommitAccounting(Cva6CPU& cpu, Cva6DynInstPtr inst){
    assert(!inst->isFault());
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

std::string instDump(Cva6DynInstPtr inst) {
    std::ostringstream ss;
    ss << *inst;
    if (inst->staticInst->isMemRef()){
        assert(inst->dreq);
        // assert(inst->dreq->req->hasPaddr());
        assert(inst->dreq->req->hasVaddr());
        ss << std::hex;
        ss << " VA=" << inst->dreq->req->getVaddr();
        if (inst->dreq->req->hasPaddr()){
            ss << " PA=" << inst->dreq->req->getPaddr();
        }
        // ss << " PTE=" << inst->dreq->req->pte;
    }
    return ss.str();
}

bool commitInst(Cva6CPU& cpu, Cva6DynInstPtr inst){
    // ThreadContext *thread = cpu.thread->getTC(); // cpu.getContext();

    DPRINTF(Cva6Execute, "executeCommit(%s)\n", *inst);
    Fault fault = inst->executeCommit(cpu, *cpu.thread);
    DPRINTF(Cva6Execute, "end executeCommit(%s)\n", *inst);

    if (fault != NoFault){
        DPRINTF(Cva6Commit, "commit: %s fault: %s\n", *inst, fault->name());
    } else {
        static int cpt = 0;
        if ((cpt++ % 100000) == 0){
            DPRINTF(Cva6CommitCpt, "LLL: %s\n", instDump(inst));
        }
        inst->exec_data.pmode = inst->readMiscReg(RiscvISA::MISCREG_PRV);
        char priv_c[] = {'U', 'S', '-', 'M'};
        // PRV_U = 0,
        // PRV_S = 1,
        // PRV_M = 3
        DPRINTF(Cva6Commit, "commit: [%c] %s\n", priv_c[inst->exec_data.pmode],
            instDump(inst));
        doInstCommitAccounting(cpu, inst);
        if (inst->traceData){
            inst->traceData->dump();
        }
        /*
        static bool linuxTrace = false;
        linuxTrace |= inst->pc->instAddr() > 0x80200000;
        if (linuxTrace){
            DPRINTF(Cva6CommitCpt, "commitLLL: %s\n", instDump(inst));
        }
        */
    }
    /* Update state */
    commit_in_uop = !inst->isFault() &&
                    inst->staticInst->isMicroop() &&
                    !inst->staticInst->isLastMicroop();

    cpu.pipeline->iq.commit(inst); /* Post-commit (for stores SQS->SQC)!*/
    cpu.pipeline->plugins.commit(inst);
    /* Update BP */
    BranchData branch = getEffectiveBranch(inst);
    if (branch.need_squash){
        if (branch.is_predicted) {
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
    resolved_branch = BranchData(); // Default is no branch
    if (checkInterrupts()){ // TODO !
        /* Signalling an interrupt this cycle */
        Fault interrupt = cpu.getInterruptController()->getInterrupt();
        assert(interrupt != NoFault);
        /* The interrupt *must* set pcState */
        cpu.getInterruptController()->updateIntrInfo();
        interrupt->invoke(cpu.getContext());
        resolved_branch = BranchData::SquashAt(cpu);
        DPRINTF(Cva6Interrupt, "Invoking interrupt: %s to PC: %s\n",
            interrupt->name(), resolved_branch);
        cpu.pipeline->sa.fixer.clear_on_it();
        flush();
        return;
    }

    /* Execute stage */
    while (inp.canPop()){
        inp.pop(); // Instruction are already pushed in fus
    }

    // Execution
    fus.advance();
    cpu.pipeline->stats.exfus += tictac();
    // /** Process result */
    cpu.pipeline->iq.execute();
    cpu.pipeline->stats.expop += tictac();

    /* Commit stage*/
    for (int i = 0; i < commitWidth; i++){ // Dual port commit
        if (!resolved_branch.isBubble()){
            break; // Block if already jump, fault ...
        }

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

        /* Check is mdpc is valid */
        /* Care inst must have a valid paddr */
        // if (cpu.pipeline->mdpc.isViolation(inst) !=
        //        !inst->break_memory_order->isBubble()){
        //     fatal("REF: %s\n testid: %d, mdpccommitid: %d\n",
        //      *inst->break_memory_order,
        //         inst->last_store_id,
        //    cpu.pipeline->mdpc.commit_table.checkLoad(inst));
        // }
        uint64_t pcstore;
        bool is_mem_violation = cpu.pipeline->mdpc.isViolation(inst, pcstore);
        /* Inst produced bad value */
        if (is_mem_violation){
            DPRINTF(Cva6Execute, "MISSPRED MEM ORDER : %s\n", *inst);
            resolved_branch = BranchData::SquashAt(cpu);
            uint64_t pcload = inst->pc->instAddr();
            cpu.pipeline->sa.scheduler.violation(pcstore, pcload);
            stats.flush ++;
            stats.flush_mdp += 1;
            cpu.pipeline->hcpred.violation(inst);
            flush();
            return; /* EARLY FLUSH : do not commit */
        }
        if (!inst->isFault() && inst->staticInst->isLoad()){
            cpu.pipeline->hcpred.confidence(inst);
        }

        /* Compare the pred base addr with the real one */
        bool misspred_addr = cpu.pipeline->dpe.post_commit(inst);
        if (misspred_addr){
            DPRINTF(Cva6Execute, "MISSPRED ADDR : %s\n", *inst);
            resolved_branch = BranchData::SquashAt(cpu);
            stats.flush ++;
            stats.flush_vp += 1;
            flush();
            return; /* EARLY FLUSH : do not commit */
        }

        /* Pre commit (that can be reversed)*/
        assert(cpu.pipeline->rob.front() == inst);
        cpu.pipeline->rob.pop(inst); /* Pre-commit */
        cpu.pipeline->mdpc.commit(inst); /* must be pre-commit ?? */
        /* Fault have to be detected before enter BC ? */
        cpu.pipeline->sa.pre_commit(inst); /* Annotate can commit */

        // bool is_serialise = !inst->isFault() && needSerial
        //     inst->isLastOpInInst() &&
        //     (inst->staticInst->isSerializeAfter() ||
        //     inst->staticInst->isSquashAfter());

        if (inst->needSerialise) { /* Inst was serialised in the scheduler*/
            // Check scheduler serialisation
            // Everything must be comitted
            assert(bc.empty());
        }

        /* Try to commit */
        bool need_squash = bc.pre_commit(inst); /* Ex CSRW ! */
        /* Some stats */
        if (inst->isASquash()){
            stats.flush ++;
            stats.flush_serialise += !inst->isFault()
                && inst->staticInst->isSerializeAfter();
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
            stats.flush_load += inst->isFault()
                 && inst->staticInst
                 && inst->staticInst->isLoad();
        }

        // Oracle: Early commit
        if (oracleEarlyCommit){
            bc.commitFunctionnal();
        }

        if (need_squash){ /* Handle flush */
            if (0){ // Oracle: Late commit and flush in place for nothing
                bc.commitFunctionnal();
            }
            if (!bc.empty()){
                cpu.pipeline->sa.fixer.apply_fix_for(inst,
                    true, true, bc.size());
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

            resolved_branch = BranchData::SquashAt(cpu);
            flush();
            return;
        } /* Otherwise take the valid path */

        // We can pre-commit the instruction
        // commitInst(inst); /* Post-commit */
        // Already done is commitInst !
        // if (need_squash){ // And not handled by bc
        //     tryToBranch(inst, resolved_branch); /* POST-commit */
        // }

        bool misspred_value = dpe.commit(inst);
        if (misspred_value){
            resolved_branch = BranchData::SquashAt(cpu);
        }
    }
    // Flush in the cycle
    if (resolved_branch.isStreamChange()){
        fatal("Unrecheable!\n");
        flush();
    }
    bc.dump();
    cpu.pipeline->stats.excommit += tictac();
}

void
Execute::flushfrom(Cva6DynInstPtr inst){
    DPRINTF(Cva6Execute, "Flush fus & inp\n");
    fus.flushfrom(inst);
    inp.flushfrom(inst);
    cpu.pipeline->rob.flushfrom(inst);
    cpu.pipeline->sa.flushfrom(inst);
    assert(inst->isBubble());
    cpu.pipeline->dpe.flush();
    bc.flush(); // Clear inflights pre-committed
}

bool
Execute::checkInterrupts()
{
    assert(FullSystem && cpu.getInterruptController());
    // TODO lastCommitWasEndOfMacroop
    if (cpu.checkInterrupts()) {
        DPRINTF(Cva6Commit, "CAN IT?\n");
        if (commit_in_uop){
            return false;
        }
        /* lsu chech in memory instruction ! */
        return cpu.pipeline->iq.canInterrupts() && bc.canInterrupts();
    }
    return false;
}


} // namespace cva6
} // namespace gem5
