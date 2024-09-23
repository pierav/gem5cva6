
#include "cpu/cva6/stage_execute.hh"

#include <functional>
#include <iomanip>

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
    if (!inst->isFault() &&
       (inst->staticInst->isReadBarrier() ||
       inst->staticInst->isWriteBarrier())){
        DPRINTF(Cva6Minicache, "CLEAR ALL FROM: %s\n", *inst);
        mc.clear_all();
        // is_serialise = true;
    }

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
    if (inst->staticInst->isLoad()){
        assert(inst->dreq);
        // assert(inst->dreq->req->hasPaddr());
        assert(inst->dreq->req->hasVaddr());
        ss << " VA=" << inst->dreq->req->getVaddr();
        if (inst->dreq->req->hasPaddr()){
            ss << " PA=" << inst->dreq->req->getPaddr();
        }
        ss << " PTE=" << inst->dreq->req->pte;
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
        DPRINTF(Cva6Commit, "commit: %s\n", instDump(inst, thread));
        doInstCommitAccounting(inst);
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
    if (checkInterrupts()){
        /* Signalling an interrupt this cycle */
        // DPRINTF(Cva6Interrupt, "Considering interrupt status from PC: %s\n",
        //     cpu.getContext()->pcState());
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

    /** Process result */
    for (Cva6DynInstPtr inst: scoreboard.getIssueQueue()){
        if (scoreboard.isInstInFu(inst)){
            // For all instructions in FUs try to complete the execution
            if (fus.canPop(inst)){
                fus.pop(inst);                  /* Compute FU and pop */
                inst->executeComplete();        /* Complete FU result */
                scoreboard.completeInst(inst);  /* Notify scoreboard */
                // TODO commit & flush ???
            }
        }
    }

    /* Commit stage*/
    Fault fault;
    for (int i = 0; i < commitWidth; i++){ // Dual port commit
        if (!resolved_branch.isBubble()){
            break; // Block if already jump, fault ...
        }
        DPRINTF(Cva6Execute, "Attempting to commit (port%d)\n", i);
        Cva6DynInstPtr inst = scoreboard.getCommitInst(i);
        if (inst->isBubble()){
            break; // No instruction to commit
        }
        DPRINTF(Cva6Execute, "begin commit: %s\n", *inst);

        if (inst->isMemRef()){
            assert(inst->dreq);
        }

        // For address prediction
        // bool misspredaddr = dpe.post_commit(inst);
        // if (misspredaddr){
        //     scoreboard.flush_value_from(inst, true);
        //     flushfrom(Cva6DynInst::bubble()); // Only Flush FUS et inps
        //     i=commitWidth;
        //     break;
        // }

        /* Commit */
        // ForwardInstData &c2e = *commit_to_issue.inputWire;
        // c2e.inst = inst;
        commitInst(inst, resolved_branch);
        scoreboard.commitInst(inst);

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

        /* RDA */
        uint64_t pc =inst->pc->instAddr();
        if (!inst->isFault()){
            for (unsigned int i = 0; i < inst->staticInst->numSrcRegs(); i++) {
                RegId reg = inst->staticInst->srcRegIdx(i);
                if (reg.classValue() != InvalidRegClass){
                    inst->exec_data.is_reg_dead[i] = rda.isRegDead(pc, reg);
                }
            }
        }

        // MC commit
        if (mc.isEnable() &&            /** MC enable */
            inst->isMemRef() &&         /** Load and stores */
            // !inst->vp_data.hit &&       /** Not vp DETER */
            inst->dreq->isBufferable() &&
            inst->dreq->req->hasPaddr() /* Not VP deter*/
        ){

            uint64_t paddr = inst->dreq->req->getPaddr();
            uint8_t size = inst->dreq->req->getSize();
            uint64_t rawdata = inst->dreq->getData();
            uint8_t *data = (uint8_t*)&rawdata;
            bool isstore = inst->staticInst->isStore();
            if (inst->dreq->isCl()){
                paddr = inst->dreq->getClPaddr();
                size = inst->dreq->getClSize();
                data = inst->dreq->getClData();
            }
            mc.commit(paddr, size, data, isstore, inst->mc_data);
            // TODO


            #if 1

            if (// !inst->vp_data.hit && // Not vp DETER
                inst->mc_data.hit && // mc hit
                inst->staticInst->isLoad()
            ){
                uint64_t real_val = inst->dreq->getData();
                uint64_t mc_val = inst->mc_data.value;
                if (real_val != mc_val){
                    fatal("MCERROR : %lx != %lx\n", real_val, mc_val);
                } else {
                    // printf("%lx == %lx\n", real_val, mc_val);
                }
            }
            #endif
        }

        // VP commit
        bool misspred = dpe.commit(inst);
        if (misspred){
            if (vpFlush){ // Flush
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
            } else { // Replay the scoreboard if needed
                Cva6DynInstPtr vilain = scoreboard.flush_value_from(inst);
                if (!vilain->isBubble()){ //
                    flushfrom(vilain); // Only Flush FUS et inps
                    i=commitWidth;
                }
            }
        }

        cpu.pipeline->lh.on_commit(inst);

        for (Plugin *plugin: plugins){
            plugin->commit(inst);
        }
        if (inst->traceData){
            inst->traceData->dump();
        }
    }
    // Flush in the cycle
    if (resolved_branch.isStreamChange()){
        flush();
    }

}

void
Execute::flushfrom(Cva6DynInstPtr inst){
    DPRINTF(Cva6Execute, "Flush fus & inp\n");
    fus.flushfrom(inst);
    inp.flushfrom(inst);
    dpe.flushfrom(inst);
}

bool
Execute::checkInterrupts()
{
    assert(FullSystem && cpu.getInterruptController());
    // TODO lastCommitWasEndOfMacroop
    if (cpu.checkInterrupts() && 1) {
        DPRINTF(Cva6Commit, "IT\n");
        /* lsu chech in memory instruction ! */
        /* TODO create canFlush() or allow speculative loads */
        Cva6DynInstPtr inst = scoreboard.getHeadInst();

        if (!inst->isBubble()){
            if (inst->isMemRef()){
            // if (inst->dreq && inst->dreq->isOutsideCpu()){
                return false;
            }
            if (inst->staticInst->isAtomic() ||
                inst->staticInst->isStoreConditional()) {
                return false;
            }
        }
        return true;
    }

    return false;
}


} // namespace cva6
} // namespace gem5
