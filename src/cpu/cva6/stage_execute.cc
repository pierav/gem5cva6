
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

void
Execute::tryToBranch(Cva6DynInstPtr inst, Fault fault, BranchData &branch){
    ThreadContext *thread = cpu.getContext();
    const std::unique_ptr<PCStateBase> pc_before(inst->pc->clone());
    std::unique_ptr<PCStateBase> target(thread->pcState().clone());

    InstSeqNum num = inst->isBubble() ? 0 : inst->id.fetchSeqNum;
    // bool taken_match = inst->pc_next_taken == inst->predictedTaken;

    bool actually_taken = inst->pc_next_taken;
    bool is_serialise = !inst->isFault() &&
        inst->isLastOpInInst() &&
        (inst->staticInst->isSerializeAfter() ||
         inst->staticInst->isSquashAfter());

    bool is_addr_unmatch = inst->triedToPredict &&
                          *inst->predictedTarget != *target;
    bool is_fault = fault != NoFault;

    bool need_squash = is_addr_unmatch ||
                       is_fault ||
                       is_serialise;

    branch = BranchData(
        inst->triedToPredict,
        need_squash,
        num,
        *target,
        actually_taken);

    DPRINTF(Branch, "tryToBranch : %s\n", branch.dump());
}

void
Execute::doInstCommitAccounting(Cva6DynInstPtr inst){
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

std::string instDump(Cva6DynInstPtr inst, ThreadContext *thread) {
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


bool
Execute::commitInst(Cva6DynInstPtr inst, BranchData &branch){
    ThreadContext *thread = cpu.thread->getTC(); // cpu.getContext();

    DPRINTF(Cva6Execute, "executeCommit(%s)\n", *inst);
    Fault fault = inst->executeCommit(cpu, *cpu.thread);
    DPRINTF(Cva6Execute, "end executeCommit(%s)\n", *inst);

    if (fault != NoFault){
        DPRINTF(Cva6Commit, "commit: %s fault: %s\n", *inst, fault->name());
        tryToBranch(inst, fault, branch);
    } else {
        static int cpt = 0;
        if ((cpt++ % 100000) == 0){
            DPRINTF(Cva6CommitCpt, "LLL: %s\n", instDump(inst, thread));
        }
        inst->exec_data.pmode = inst->readMiscReg(RiscvISA::MISCREG_PRV);
        char priv_c[] = {'U', 'S', '-', 'M'};
        //     PRV_U = 0,
        // PRV_S = 1,
        // PRV_M = 3
        DPRINTF(Cva6Commit, "commit: [%c] %s\n", priv_c[inst->exec_data.pmode],
            instDump(inst, thread));
        doInstCommitAccounting(inst);
        if (inst->traceData){
            inst->traceData->dump();
        }
        tryToBranch(inst, fault, branch);
        /*
        static bool linuxTrace = false;
        linuxTrace |= inst->pc->instAddr() > 0x80200000;
        if (linuxTrace){
            DPRINTF(Cva6CommitCpt, "commitLLL: %s\n", instDump(inst, thread));
        }
        */
    }
    return fault != NoFault;
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

        DPRINTF(Cva6Interrupt, "Invoking interrupt: %s to PC: %s\n",
            interrupt->name(), cpu.getContext()->pcState());

        std::unique_ptr<PCStateBase> target(
            cpu.getContext()->pcState().clone());
        InstSeqNum num = 0;
        resolved_branch = BranchData(
            false, //
            true, // Need squash
            num,
            *target,
            true // Unused
        );
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
    Fault fault;
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
            resolved_branch = BranchData::SquashAt(
                cpu.getContext()->pcState());
            uint64_t pcload = inst->pc->instAddr();
            cpu.pipeline->sa.scheduler.violation(pcstore, pcload);
            flush();
            return; /* EARLY FLUSH : do not commit */
        }

        /* Compare the pred base addr with the real one */
        bool misspred_addr = cpu.pipeline->dpe.post_commit(inst);
        if (misspred_addr){
            DPRINTF(Cva6Execute, "MISSPRED ADDR : %s\n", *inst);
            resolved_branch = BranchData::SquashAt(
                cpu.getContext()->pcState());
            flush();
            return; /* EARLY FLUSH : do not commit */
        }

        /* Commit everyrhings */
        commitInst(inst, resolved_branch);
        cpu.pipeline->iq.commit(inst);
        assert(cpu.pipeline->rob.front() == inst);
        cpu.pipeline->rob.pop(inst);
        cpu.pipeline->sa.commit(inst);
        cpu.pipeline->mdpc.commit(inst);
        /* Check if there is memory order violation */
        // cpu.pipeline->iq.markMemoryViolation(inst);
        // TODO: replay from load !

        // iq.commit(inst);
        // cpu.pipeline->rob.pop(inst);

        /* Checker */
        if (!inst->isFault() && inst->staticInst->isMemRef()){
            /* Request values */
            uint64_t addr = inst->dreq->getPaddr();
            uint8_t size = inst->dreq->getSize();
            uint64_t value = inst->dreq->getData();
            if (inst->dreq->isBufferable()){ /* Bufferable load or store */
                if (inst->staticInst->isLoad()){ /* Load checker */
                    bool isconst = memcheck.check_load(addr, size, value);
                    inst->exec_data.is_const_load = isconst;
                } else { /* Update store*/
                    bool indempotant = memcheck.check_store(addr, size, value);
                    inst->exec_data.is_silent_store = indempotant;
                }
            } else { /* Not bufferable */
                memcheck.invalidate(addr);
            }
        }
        // Reg checker
        {
            static ArchRegFile<char> rfinit;
            static ArchRegFile<uint64_t> rf;
            if (!inst->isFault()){
                // 0) Check src
                for (auto& reg: inst->regs_src_phy){
                    if (!rfinit[reg]){ // For simpoint
                        rf[reg] = reg.value;
                        rfinit[reg] = true;
                    }
                    fatal_if(rf[reg] != reg.value,
                        "reg %s must be equal to %lx not %lx\n",
                        reg, rf[reg], reg.value);
                }
                // 1) Apply dsts
                for (auto& reg: inst->regs_dst_phy){
                    rf[reg] = reg.value;
                    rfinit[reg] = true;
                }
            }
        }
        bool misspred_value = dpe.commit(inst);
        if (misspred_value){
            ThreadContext *thread = cpu.getContext();
            resolved_branch = BranchData::SquashAt(thread->pcState());
        }
        // VP commit
        # if 0
        if (misspred_value){
            if (vpFlush){ //  vpFlush Flush
                resolved_branch = BranchData::SquashAt(
                    cpu.getContext()->pcState());
                flush();
                return;  /* LATE FLUSH : commit then flush */
            } else { // Replay the scoreboard if needed
                fatal("Unimp!\n");
                #if 0
                Cva6DynInstPtr vilain = scoreboard.flush_value_from(inst);
                if (!vilain->isBubble()){ //
                    flushfrom(vilain); // Only Flush FUS et inps
                    i=commitWidth;
                }
                #endif
            }
        }
        #endif

        for (Plugin *plugin: plugins){
            plugin->commit(inst);
        }

    }
    // Flush in the cycle
    if (resolved_branch.isStreamChange()){
        flush();
    }
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
}

bool
Execute::checkInterrupts()
{
    assert(FullSystem && cpu.getInterruptController());
    // TODO lastCommitWasEndOfMacroop
    if (cpu.checkInterrupts() && 1) {
        DPRINTF(Cva6Commit, "IT\n");
        /* lsu chech in memory instruction ! */
        return cpu.pipeline->iq.canInterrupts();
    }

    return false;
}


} // namespace cva6
} // namespace gem5
