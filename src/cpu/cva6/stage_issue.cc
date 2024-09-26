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
#include "cpu/op_class.hh"
#include "debug/Cva6Issue.hh"

namespace gem5 {
namespace cva6 {

void
IssueUnit::evaluate(){
    int nb_issued = 0;
    int cnt_push_load = 0;
    for (int i = 0; i < nb_issue_port; i++){ // Try to issue instruction
        bool is_over_serialise;
        bool is_ready;
        Cva6DynInstPtr inst = scoreboard.getIssueInst(0,
            is_over_serialise, is_ready);

        /* 0) Is instruction present ?*/
        if (inst->isBubble() || is_over_serialise){
            DPRINTF(Cva6Issue, "(port %d) Frontend stall...\n", i);
            stats.issue_stall_front += 1;
            break;
        }
        // DPRINTF(Cva6Issue, "(port %d) process %s\n", i, *inst);
        if (!inst->issue_start_ts){
            inst->issue_start_ts = cpu.curCycle();
        }

        if (!isLamdbaOrderOk(inst)){
            DPRINTF(Cva6Issue,
                "(port %d) Lambda Order stall ... %s\n",i, *inst);
            break;
        }

        /* 1) Is operands ready ? */
        if (!is_ready){
            DPRINTF(Cva6Issue,
                "(port %d) Read operands stall... %s\n", i, *inst);
            stats.issue_stall_iro += 1;
            break;
        }

        /* 2) Is available FU ? */
        if (!fus.canPush(inst)){
            DPRINTF(Cva6Issue,
                "(port %d) Functionnal U stall... %s\n", i, *inst);
            stats.issue_stall_fu += 1;
            break;
        }

        /* 3) Is FU contention ? */
        if (!inst->isFault() && inst->staticInst->isLoad()){
            if (cnt_push_load >= 2){
                DPRINTF(Cva6Issue, "(port %d) LOAD stall... %s\n", i, *inst);
                stats.issue_stall_fu += 1;
                break; // No available FU
            } else {
                cnt_push_load += 1;
            }
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
        /* markup dst registers as busy */
        scoreboard.issueInst(inst, *cpu.thread);
        /* Initiate the instruction (reg src are available)*/
        inst->executeInitiate();

        bool need_execution = true;
        if (need_execution){
            /* Push in fu */
            fus.push(inst);
            // out.push(inst);
        } else { // VP hit @
            scoreboard.completeInst(inst);  /* Notify scoreboard */
        }

        /* Some statistics */
        if (!inst->isFault()){
            stats.typeIssued[0][inst->staticInst->opClass()]++;
        }
        nb_issued += 1;
    }
    stats.numIssued.sample(nb_issued);
}
static Cva6DynInstPtr pinst = Cva6DynInst::bubble();

void
Issue::evaluate() {
    /* Flush on missprediction */
    if (resolved_branch.isStreamChange()) {
       flush();
       return;
    }

    /* Insert decoded instructions in the selected scoreboard */
    while (inp.canPop()){
        Cva6DynInstPtr inst = inp.front();
        /* Select IQ */
        bool inlambda = inst->l_data.is_predicted;
        IssueUnit &iq = cpu.pipeline->getIq(inlambda);
        if (iq.canPush()){
            /* Insert uOp in IQ when lambda start ! */
            if (inst->l_data.is_predicted_first){
                if (!cpu.pipeline->getIq(FAST_IQ).canPush()){
                    break;
                }
                // uOp insertion in fast IQ
                assert(pinst->isBubble()); // No lambda overlap !
                pinst = cpu.pipeline->lh.newPredInst(inst);
                cpu.pipeline->getIq(FAST_IQ).push(pinst);
            }
            inp.pop();
            iq.push(inst);
            cpu.pipeline->rob.push(inst);
        } else { // In order issue
            break;
        }

        /* Insert uOp in ROB when lambda end */
        if (inst->l_data.is_predicted_last){
            assert(!pinst->isBubble());
            cpu.pipeline->rob.push(pinst);
            pinst = Cva6DynInst::bubble();
        }
    }

    /* Fast IQ cannot be blocked and take FU priority over checker */
    cpu.pipeline->getIq(FAST_IQ).evaluate();
    /* Handle Checker locking */
    if (cpu.pipeline->getIq(FAST_IQ).allowLambdaIq()){
        cpu.pipeline->getIq(LAMBDA_IQ).evaluate();
    }

    /* Dump Scoreboards */
    cpu.pipeline->iq0.dump();
    cpu.pipeline->iq1.dump();

    /* Tick (remove commited isntructions ) */
    cpu.pipeline->iq0.tick();
    cpu.pipeline->iq1.tick();
}

void
Issue::flush(){
    DPRINTF(Cva6Issue, "Flush Scoreboard and inp\n");
    cpu.pipeline->iq0.flush();
    cpu.pipeline->iq1.flush();
    inp.flush();
    pinst = Cva6DynInst::bubble();
}


} // namespace cva6
} // namespace gem5
