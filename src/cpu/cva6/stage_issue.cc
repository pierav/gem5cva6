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
Issue::evaluate() {
    /* Flush on missprediction */
    if (resolved_branch.isStreamChange()) {
       flush();
       return;
    }

    /* Insert decoded instructions in the scoreboard */
    while (inp.canPop() && scoreboard.canPush()){
        Cva6DynInstPtr inst = inp.pop();
        inst->stage_issue_enter = true;
        scoreboard.pushInst(inst);
    }

    /* Issue instructions */
    bool used_fu[fus.nbFu()] = { false };

    unsigned int cnt_push_load = 0;
    // unsigned int cnt_push_store = 0;

    assert(out.empty());
    int nb_issued = 0;
    for (int i = 0; i < nb_issue_port; i++){
        // DPRINTF(Cva6Issue, "Attempting to issue (port %d)\n", i);
        // Try to issue instruction
        Cva6DynInstPtr inst = scoreboard.getIssueInst(0);
        if (inst->isBubble()){
            DPRINTF(Cva6Issue, "(port %d) Scoreboard stall...\n", i);
            break; // No instruction to issue
        }
        if (!fus.canPush(inst)){
            DPRINTF(Cva6Issue, "(port %d) FU stall...\n", i);
            break; // No available FU
        }

        if (!inst->isFault() && inst->staticInst->isLoad()){
            if (cnt_push_load >= 2){
                DPRINTF(Cva6Issue, "(port %d) LOAD stall...\n", i);
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

        /* Decorate the inst with FU details */
        inst->fuIndex = fus.getValidFuIndex(inst);
        // if (used_fu[inst->fuIndex] && ){
        //     DPRINTF(Cva6Issue, "(port %d) LSU stall...\n", i);
        //     break; // FU already used
        // }
        used_fu[inst->fuIndex] = true;

        /* Can insert the instruction into this FU */
        DPRINTF(Cva6Issue, "(port %d) Issuing %s FU: %d\n", i,
            *inst, inst->fuIndex);
        /* Forward src and markup dst registers as busy */
        scoreboard.issueInst(inst, *cpu.thread);

        /* Initiate the instruction (reg src are available)*/
        inst->executeInitiate();

        bool need_execution = true;
        need_execution = dpe.issue(inst);

        if (need_execution){
            /* Push in fu */
            fus.push(inst);
            out.push(inst);
        } else { // VP hit @
            scoreboard.completeInst(inst);  /* Notify scoreboard */
            // Cycles delta = inst->issue_ts  - inst->issue_start_ts;
            // printf("DELTA : %d\n", delta);
        }
        /* Some statistics */
        if (!inst->isFault()){
            stats.typeIssued[0][inst->staticInst->opClass()]++;
        }
        nb_issued += 1;
    }
    stats.numIssued.sample(nb_issued);


    scoreboard.tick();
}

void
Issue::flush(){
    DPRINTF(Cva6Issue, "Flush Scoreboard and inp\n");
    scoreboard.flush();
    inp.flush();
    dpe.flush();
}


} // namespace cva6
} // namespace gem5
