/**
 * @file stage_issue.hh
 * @author Pierre Ravenel (pravenel@kalray.eu)
 * @brief
 * @version 0.1
 * @date 2023-05-25
 *
 */
#include "cpu/cva6/stage_issue.hh"
#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/exec_context.hh"
#include "cpu/cva6/pipeline.hh"
#include "debug/Cva6Issue.hh"

namespace gem5 {
namespace cva6 {

const char* OCSNames[] = {"NoOp", "Alu", "Fpu", "Control", "Read", "Write"};

void
IssueUnit::evaluate(){
    int nb_issued = 0;
    int readarf = 0;
    int readfrf = 0;
    int cnt_push_load = 0;

    /* Do dispatch before if needed */
    scoreboard.dispatch_before();

    for (int i = 0; i < nb_issue_port; i++){ // Try to issue instruction
        bool is_over_serialise, is_raw, is_waw;
        Cva6DynInstPtr producer = Cva6DynInst::bubble();
        Cva6DynInstPtr inst = scoreboard.getIssueInst(
            is_over_serialise, is_raw, producer, is_waw);

        /* -1) Outdated : with RR, cannot block */
        // if (!scoreboard.canPush()){
        //     DPRINTF(Cva6Issue, "(port %d) Scoreboard full...\n", i);
        //     stats.issue_stall_full += 1;
        //     break;
        // }

        /* 0) Is instruction present ?*/
        if (inst->isBubble()){
            DPRINTF(Cva6Issue, "(port %d) Frontend stall...\n", i);
            stats.issue_stall_front += 1;
            break;
        }

        /* 0.1) is_over_serialise ? */
        if (is_over_serialise){
            DPRINTF(Cva6Issue, "(port %d) is over serial stall... %s\n",
                i, *inst);
            stats.issue_stall_serialise += 1;
            break;
        }

        // DPRINTF(Cva6Issue, "(port %d) process %s\n", i, *inst);
        if (!inst->issue_start_ts){
            inst->issue_start_ts = cpu.curCycle();
        }

        /* 1) RaW : Is operands ready ? */
        if (is_raw){
            assert(!producer->isBubble());
            DPRINTF(Cva6Issue,
                "(port %d) RaW stall ............ %s\n", i, *inst);
            // DPRINTF(Cva6Issue,
            //     "(port %d) Producer is %s\n", i, *producer);
            stats.issue_stall_iro += 1;
            stats.typeStallProducerReg[0][getOcs(producer)]++;
            break;
        }

        /* 1.1) WaW */
        // TODO care OoO Commit ! Ignore WaW for now
        if (is_waw){
            DPRINTF(Cva6Issue,
                "(port %d) WaW stall ............ %s\n", i, *inst);
            stats.issue_stall_waw += 1;
            break;
        }

        /* 2) Is available FU ? */
        if (!fus.canPush(inst)){
            DPRINTF(Cva6Issue,
                "(port %d) Functionnal U stall... %s\n", i, *inst);
            stats.issue_stall_fu += 1;
            stats.typeStallFU[0][getOcs(inst)]++;
            break;
        }

        /* 3) Is FU contention ? */
        if (!inst->isFault() && inst->staticInst->isLoad()){
            if (cnt_push_load >= loadPerCycle){
                DPRINTF(Cva6Issue, "(port %d) LOAD stall... %s\n",
        i, *inst);
                stats.issue_stall_fu += 1;
                break; // No available FU
            }
            cnt_push_load += 1;
        }
        /*
        if (!inst->isFault() && inst->staticInst->isStore()){
            if (cnt_push_store){
                DPRINTF(Cva6Issue, "(port %d) STORE stall...\n", i);
                break; // No available FU
            } else {
                cnt_push_store += 1;
            }
        }
        */

        /* Finnaly issue the instruction */
        inst->issue_ts = cpu.curCycle();
        if (!inst->isFault() && inst->staticInst->isLoad()){
            stats.issue_stall_raw.sample(
                    inst->issue_ts - inst->issue_start_ts);
        }
        stats.issue_pass +=1;

        /* Update stats ans status (must be called before exInitiate()) */
        scoreboard.issueInst(inst);

        /* Initiate the instruction (reg src are available)*/
        /* May create fault ! */
        inst->executeInitiate();

        /* Decorate the inst with FU details */
        inst->fuIndex = fus.getValidFuIndex(inst);
        // if (used_fu[inst->fuIndex] && ){
        //     DPRINTF(Cva6Issue, "(port %d) LSU stall...\n", i);
        //     break; // FU already used
        // }
        // used_fu[inst->fuIndex] = true;

        /* Can insert the instruction into this FU */
        DPRINTF(Cva6Issue, "(port %d) Issuing %s FU: %d\n", i,
            *inst, inst->fuIndex);

        bool need_execution = cpu.pipeline->dpe.issue(inst);
        if (need_execution){ /* Push in fu */
            fus.push(inst);
        } else { // VP hit @
            scoreboard.completeInst(inst);  /* Notify scoreboard */
        }

        /* Mdp things */
        if (!inst->isFault() && inst->staticInst->isStore()){
            cpu.pipeline->mdp.popStore(inst->pc->instAddr(), inst);
        }

        /* Some statistics */
        if (!inst->isFault()){
            stats.typeIssued[0][getOcs(inst)]++;
        }
        for (PhysicalReg& reg: inst->regs_src_phy){
            readarf += !reg.isRenammedValid;
            readfrf += reg.isRenammedValid;
        }
        nb_issued += 1;
    }
    stats.issue_stall_port += nb_issued == nb_issue_port;

    stats.numIssued.sample(nb_issued);
    stats.numIssuedReadFRF.sample(readfrf);
    stats.numIssuedReadARF.sample(readarf);

    /* Do dispatch after (1 cycle delay)*/
    scoreboard.dispatch();
}

void
Issue::evaluate() {
    cpu.pipeline->iq.evaluate();
    cpu.pipeline->iq.dump();
    cpu.pipeline->iq.tick();
}

void
Issue::flushfrom(Cva6DynInstPtr inst){
    DPRINTF(Cva6Issue, "Flush Scoreboard and inp\n");
    inp.flushfrom(inst);
    cpu.pipeline->iq.dump();
    cpu.pipeline->iq.flushfrom(inst);
}


} // namespace cva6
} // namespace gem5
