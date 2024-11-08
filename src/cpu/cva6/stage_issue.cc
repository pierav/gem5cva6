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
#include "cpu/op_class.hh"
#include "debug/Cva6Issue.hh"

namespace gem5 {
namespace cva6 {

const char* OCSNames[] = {"NoOp", "Alu", "Fpu", "Control", "Read", "Write"};

bool
IssueUnit::robGetRegFunctionnal(uint64_t pos, RegId reg_src, RegVal &fwval,
    Cva6DynInstPtr &instfw){
    /* Skip unforwardable registers */
    if (reg_src.classValue() == MiscRegClass ||
        reg_src.classValue() == InvalidRegClass){
        fwval = 0;
        return true;
    }
    for (int i = pos - 1; i >= 0; i--){
        Cva6DynInstPtr i2 = cpu.pipeline->rob[i];
        instfw = i2;
        if (i2->isFault()){ // Stall after fault
            return false;
        }
        for (uint8_t i = 0; i < i2->numDstRegs(); i++) {
            if (i2->dstRegIdx(i) == reg_src){ // Hit register
                //DPRINTF(Cva6Scoreboard, "Match FW %s\n", *inst);
                if (i2->reg_dst_val_valid[i]){ /* Ready -> Forward */
                    fwval = i2->getDstRegOperand(i);
                    return true;
                } else { /* In use */
                    return false;
                }
            }
        }
    }
    /* Register is not in flight, use reg file */
    fwval = cpu.thread->getReg(reg_src);
    return true;
}

bool
IssueUnit::isLamdbaOrderOk(Cva6DynInstPtr inst){
    #if 0
    /* Only the first lambda in IQ if present can be issued
    * | OK
    * E OK
    * S !OK
    * | !OK
    * E !OK
    */
    for (Cva6DynInstPtr i2 : scoreboard.getIssueQueue()){
        if (i2 == inst){
            return true;
        }
        if (i2->l_data.is_predicted_last){
            return false;
        }
    }
    return true;
    #endif

    /* Fault does not have register dependancies */
    if (inst->isFault()){
        return true;
    }

    // /* If fast IQ, there is no deps */
    // if (&cpu.pipeline->getIq(FAST_IQ) == this){
    //     return true;
    // }

    /* Check if there is no non-issued stores */
    Scoreboard &fast_sb = cpu.pipeline->getIq(FAST_IQ).scoreboard;
    if (!inst->isFault() &&
        inst->isMemRef() &&
        fast_sb.isUnissedStoreBefore(inst)){
        DPRINTF(Cva6Issue, "LAMBDA unresolved mem dep\n");
        return false;
    }

    // Find instruction position in rob
    int pos = cpu.pipeline->rob.size(); // Default is outside sb
    for (int i = 0; i < cpu.pipeline->rob.size(); i++){
        assert(!cpu.pipeline->rob[i]->isBubble());
        if (cpu.pipeline->rob[i]->isAfterOrEqual(inst)){
            pos = i;
            break;
        }
    }

    /* Check and Forward register from the other iq */
    uint8_t num_srcs = inst->staticInst->numSrcRegs();
    for (uint8_t src_index = 0; src_index < num_srcs; src_index++){
        RegId reg_src = inst->staticInst->srcRegIdx(src_index);
        RegVal fwval;
        Cva6DynInstPtr instfw = Cva6DynInst::bubble();
        if (robGetRegFunctionnal(pos, reg_src, fwval, instfw)){
            inst->setSrcRegOperand(src_index, fwval);
            continue;
        } else {
            if (!inst->isFault() &&
               !instfw->isBubble() &&
               !instfw->isFault() &&
               instfw->staticInst->isLoad()){
                stats.typeStallOnLoad[0][getOcs(inst)]++;
            }
            return false;
        }
    }
    /* Finally all conditions are met ! */
    return true;
}

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

        /* If fast IQ, there is no deps */
        if (&cpu.pipeline->getIq(LAMBDA_IQ) == this){
            if (!inst->isFault() &&
               inst->isMemRef() &&
               !inst->dreq->isBufferable()){
                need_execution = false;
                inst->dreq->prefetch_mode_failed = true;
            }
        }
        if (need_execution){
            /* Push in fu */
            fus.push(inst);
            // out.push(inst);
        } else { // VP hit @
            scoreboard.completeInst(inst);  /* Notify scoreboard */
        }

        /* Some statistics */
        if (!inst->isFault()){
            stats.typeIssued[0][getOcs(inst)]++;
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
            /* have 1 slot remaining for uOp */
            if (inst->l_data.is_predicted_last &&
                !cpu.pipeline->getIq(FAST_IQ).canPush()){
                break;
            }
            inp.pop();
            iq.push(inst);
            cpu.pipeline->rob.push(inst);
        } else { // In order issue
            break;
        }

        /* Insert uOp in ROB / IQ when lambda end */
        if (inst->l_data.is_predicted_last){
            assert(cpu.pipeline->getIq(FAST_IQ).canPush());
            // uOp insertion in fast IQ
            pinst = cpu.pipeline->lh.newPredInst(inst);
            pinst->id.fetchSeqNum = inst->id.fetchSeqNum;
            cpu.pipeline->getIq(FAST_IQ).push(pinst);
            cpu.pipeline->rob.push(pinst);
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
}


} // namespace cva6
} // namespace gem5
