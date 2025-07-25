/**
 * @file stage_fetch2.cc
 * @author Pierre Ravenel (pravenel@kalray.eu)
 * @brief
 * @version 1.0
 * @date 2023-05-25
 *
 */

#include "cpu/cva6/stage_fetch2.hh"

#include <string>

#include "arch/generic/decoder.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/cva6/pipeline.hh"
#include "cpu/pred/bpred_unit.hh"
#include "cpu/pred/simple_indirect.hh"
#include "cpu/pred/tage.hh"
#include "cpu/pred/tage_sc_l.hh"
#include "debug/Branch.hh"
#include "debug/Cva6Fetch.hh"
#include "debug/Cva6X.hh"
#include "debug/Decode.hh"
#include "debug/Fetch.hh"

namespace gem5 {
namespace cva6 {

Cva6DynInstPtr
UDecoder::decodeInst(Cva6DynInstPtr inst,
    bool &input_finished){
    // DPRINTF(Decode, "Inst %s\n", *inst);
    // Default behavior: nothing to decode
    Cva6DynInstPtr output_inst = inst;
    input_finished = true;

    if (inst->isFault()) {
        // DPRINTF(Decode, "Fault being passed: %d\n",
        //  inst->getFault()->name());
        // return output_inst;
    } else {
        StaticInstPtr si = inst->staticInst;
        if (si->isMacroop()) {
            input_finished = false;

            /* Set up PC for the next micro-op emitted */
            if (!inMacroop) {
                set(microopPC, *inst->pc);
                inMacroop = true;
            }

            /* Get the micro-op static instruction from the si. */
            StaticInstPtr sui = si->fetchMicroop(microopPC->microPC());
            output_inst = new Cva6DynInst(&cpu, sui, &microopPC);

            /* Allow a predicted next address only on the last microop */
            if (sui->isLastMicroop()) {
                output_inst->predictedTaken = inst->predictedTaken;
                set(output_inst->predictedTarget, inst->predictedTarget);
            }

            DPRINTF(Decode, "Microop decomposition lastMicroop: %s microopPC:"
                " %s inst: %d\n", (sui->isLastMicroop() ?
                    "true" : "false"),
                *microopPC,
                *output_inst);

            /* Acknowledge that the si isn't mine,
                it's my parent macro-op's */
            sui->advancePC(*microopPC);

            /* Step input if this is the last micro-op */
            if (sui->isLastMicroop()) {
                inMacroop = false;
                input_finished = true;
            }
        }
    }

    /** Set execSeqNum of output_inst */
    output_inst->id.execSeqNum = execSeqNum;
    /** Add the tracing data to an instruction. */
    output_inst->traceData = cpu.getTracer()->getInstRecord(curTick(),
        cpu.getContext(),
        output_inst->staticInst, *output_inst->pc, inst->staticInst);
    if (inst->traceData){
        inst->traceData->setFetchSeq(inst->id.execSeqNum);
    }
    /** Step to next sequence number */
    execSeqNum++;
    return output_inst;
}

void
Fetch2::updateBranchPrediction(const BranchData &branch){
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
}

bool
Fetch2::computeHighConf(Cva6DynInstPtr& inst){
    if (inst->staticInst->isReturn()){
        /* Force HC for returns. Ras predictions are globally true*/
        inst->isHighConf = true;
    } else if (inst->staticInst->isCondCtrl()){
        /* Retrieve the TAGE confidence */
        using pred_t = branch_prediction::TAGEBase;
        inst->isHighConf = pred_t::last_high_conf;
        inst->predFromBim = pred_t::last_pred_from_bim;
    } else {
        assert(inst->staticInst->isUncondCtrl());
        /* Perform a prediction for Uncond */
        if (inst->staticInst->isDirectCtrl()){
            inst->isHighConf = true;
        } else {
           // inst->isHighConf = false;
           // inst->isHighConf = cpu.pipeline->hcpred.predictIsHC(inst);
           using ipred_t = branch_prediction::SimpleIndirectPredictor;
           inst->isHighConf = ipred_t::last_high_conf;
        }
    }
    return inst->isHighConf;
}

void
Fetch2::predictBranch(Cva6DynInstPtr inst, BranchData &branch){
    assert(!inst->predictedTaken); /* Only 1 prediction */
    StaticInstPtr si = inst->staticInst;
    cpu.pipeline->sa.fixer.before_predict(inst);

    /* Only branch and syscall ?? */
    if (si->isControl() || si->isSyscall()){

        std::unique_ptr<PCStateBase> inst_pc(inst->pc->clone());

        /* Tried to predict */
        DPRINTF(Branch, "Trying to predict for inst: %s\n", *inst);
        inst->triedToPredict = true;

        inst->predictedTaken = cpu.pipeline->bp.predict(
            si, inst->id.fetchSeqNum, *inst_pc, 0);

        /* Care BTB miss that set predictTaken to 0 !*/

        /* Force a valid branch if Uncond Direct */
        // /!\ No more need as it's done in the BpredUnit !
        if (si->isUncondCtrl() && si->isDirectCtrl()){
            /* BUG JAL ! Must use RAS !*/
            inst->predictedTaken = true;
            // set(inst_pc, inst->pc);
            set(inst_pc, si->branchTarget(*inst->pc));
            DPRINTF(Branch, "Force prediction for %s : %lx\n",
                *inst, inst_pc->instAddr());
        }
        /* Compute HC */
        if (si->isControl()){
            computeHighConf(inst);
        }
        set(inst->predictedTarget, inst_pc);

        /* Fetch from a valid PC if ctrl flow changes */
        if (inst->predictedTaken){
            branch = BranchData(inst->triedToPredict,
                        inst->predictedTaken, // Squash if taken !
                        inst->id.fetchSeqNum,
                        *inst->predictedTarget,
                        inst->predictedTaken);
        }
        DPRINTF(Branch, "Prediction for inst %d is %s\n", *inst, branch);
    } else {
        DPRINTF(Branch, "Not attempting prediction for inst: %s\n", *inst);
    }

    /* For a fixup (Anti deadlock) */
    cpu.pipeline->sa.fixer.after_predict(inst);
}

void
Fetch2::output_inst(Cva6DynInstPtr inst){
    /* Fetch and prediction sequence numbers originate here */
    inst->id.fetchSeqNum = fetchInfo.fetchSeqNum;
    /* RDA */
    uint64_t pc =inst->pc->instAddr();
    if (!inst->isFault()){
        for (unsigned int i = 0; i < inst->staticInst->numSrcRegs(); i++) {
            RegId reg = inst->staticInst->srcRegIdx(i);
            if (reg.classValue() != InvalidRegClass){
                inst->exec_data.is_reg_dead[i] =
                    cpu.pipeline->rda.isRegDead(pc, reg);
            }
        }
    }
    if (cpu.pipeline->fus.isUnimp(inst)){
        inst->setFaultFrontend(NeverCommitFault::fault());
    }
    /* BB idx */
    static uint64_t bbcnt = 0;
    inst->bb_idx = bbcnt;
    bbcnt += !inst->isFault() && inst->staticInst->isControl();
    /* REG API */
    if (!inst->isFault()){
      /* Rename instruction : default is no renamming */
      BinaryRegisterFile rf;
      for (uint8_t i = 0; i < inst->staticInst->numSrcRegs(); i++) {
        RegId regid = inst->staticInst->srcRegIdx(i);
        if ((regid.classValue() != InvalidRegClass) && !rf.isSet(regid)){
          PhysicalReg reg(regid);
          reg.is_reg_dead = inst->exec_data.is_reg_dead[i];
          inst->regs_src_phy.push_back(reg);
          rf.set(regid);
        }
      }
      rf.clear();
      for (uint8_t i = 0; i < inst->staticInst->numDestRegs(); i++) {
        RegId regid = inst->staticInst->destRegIdx(i);
        if ((regid.classValue() != InvalidRegClass) && !rf.isSet(regid)){
          inst->regs_dst_phy.push_back(PhysicalReg(regid));
          rf.set(regid);
        }
      }
      /* Annotate missing Reg Dead */
      for (auto& reg: inst->regs_src_phy){
        if (rf.isSetRaw(reg.virt_reg_idx)){ /* Rf contains rd regs */
          reg.is_reg_dead = true;
        }
      }
    }
    inst->ts_fetch_completed = cpu.curCycle();
    /* Predict MDP */
    /* Predict memory dependancies */
    if (!inst->isFault() && inst->staticInst->isMemRef()){
        cpu.pipeline->mdp.checkInst(inst->pc->instAddr(), &inst->mdpinst);
    }
    /* mark store if needed. Predict before update. */
    if (!inst->isFault() && inst->staticInst->isStore()){
        cpu.pipeline->mdp.pushStore(inst->pc->instAddr(), inst);
    }
    out.push(inst);
    DPRINTF(Cva6Fetch, "Fetched/PreDecoded %s\n", *inst);
}

void
Fetch2::evaluate(){
    predictionOut = BranchData::bubble();

    /* update local branch prediction structures */
    updateBranchPrediction(resolved_branch);

    /* Flush on missprediction */
    if (resolved_branch.isStreamChange()) {
        flush();
        return;
    }

    int fetched_inst = 0;
    if (inp.size() && out.canPush()){
        ForwardLineData* line_in = inp.front();
        ThreadContext *thread = cpu.getContext();
        InstDecoder *decoder = thread->getDecoderPtr();

        /* Set the PC if the stream changes. */
        if (!fetchInfo.havePC) {
            fetchInfo.havePC = true;
            set(fetchInfo.pc, line_in->pc);
            decoder->reset();
            DPRINTF(Fetch, "GOT NEW STREAM: %s\n", *line_in->pc);
        }

        if (line_in->isFault()) {
            DPRINTF(Fetch, "gen instr fault...\n");
            /* Make a new instruction */
            Cva6DynInstPtr inst = new Cva6DynInst(&cpu, &fetchInfo.pc,
                line_in->fault);
            output_inst(inst);
            /* Step to next sequence number */
            fetchInfo.fetchSeqNum++;
            fetched_inst++;
        } else {
            DPRINTF(Fetch, "gen instr ...\n");
            uint8_t *line = line_in->line;
            uint8_t lineAlign[line_in->lineWidth + sizeof(uint32_t)] = { 0 };
            bool aligned = line_in->lineBaseAddr  % sizeof(uint32_t) == 0;
            memcpy(lineAlign + (aligned ? 0 : sizeof(uint32_t)/2),
                   line, line_in->lineWidth);
            size_t lineAlignWidth = line_in->lineWidth
                                    + (aligned ? 0 : sizeof(uint32_t)/2);
            for (size_t i = 0;
                i + decoder->moreBytesSize() <= lineAlignWidth;
                i += decoder->moreBytesSize()
            ){
                /* Append data in decoder */
                memcpy(decoder->moreBytesPtr(), lineAlign + i,
                       decoder->moreBytesSize());
                decoder->moreBytes(*fetchInfo.pc, 0);
                DPRINTF(Fetch, "Offering MachInst to decoder\n");
                // Decode
                while (decoder->instReady()){
                    // DPRINTF(Fetch, "Inst @%x\n",fetchInfo.pc->instAddr());
                    /* Note that the decoder can update the given PC.*/
                    StaticInstPtr si = decoder->decode(*fetchInfo.pc);
                    /* Make a new instruction */
                    Cva6DynInstPtr inst = new Cva6DynInst(&cpu, si,
                        &fetchInfo.pc);
                    inst->id.fetchSeqNum = fetchInfo.fetchSeqNum;
                    // DPRINTF(Fetch, "decoded inst %s\n", *inst);

                    /* Advance PC for the next instruction */
                    fetchInfo.pc->uReset();
                    si->advancePC(*fetchInfo.pc);

                    /* Predict any branches necessary */
                    predictBranch(inst, predictionOut);

                    /* uOP decomposition */
                    bool inputdone;
                    Cva6DynInstPtr uinst;
                    do{ /* Decomposition & push */
                        uinst = udecoder.decodeInst(inst, inputdone);
                        output_inst(uinst);
                    } while (!inputdone);
                    /* Step to next sequence number */
                    fetchInfo.fetchSeqNum++;
                    fetched_inst++;

                    /* Stop if jump is taken */
                    if (!predictionOut.isBubble()){
                        break;
                    }
                    if (decoder->needMoreBytes()){
                        break;
                    }
                    decoder->moreBytes(*fetchInfo.pc, 0);
                }
                if (!predictionOut.isBubble()){
                    break;
                }
            }
        }
        inp.pop();
    }

    stats.nisnDist.sample(fetched_inst);
    /* F2 -> F1 unlatched */
     if (!predictionOut.isBubble()){
        fetchInfo.havePC = false;
    }
}

void
Fetch2::flush(){
    DPRINTF(Cva6Fetch, "Flush\n");
    inp.flush();
    fetchInfo.havePC = false;
    udecoder.flush();
}

} // namespace cva6
} // namespace gem5
