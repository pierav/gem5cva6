/**
 * @file scoreboard.hh
 * @author Pierre Ravenel (pravenel@kalrayinc.com)
 * @brief
 * @version 0.1
 * @date 2023-05-25
 */

#include "cpu/cva6/scoreboard.hh"

#include "cpu/reg_class.hh"
#include "debug/Cva6Scoreboard.hh"

namespace gem5 {
namespace cva6 {

static const std::string stateStr[] = {
    "FREE", "IN_USE", "FWABLE", "COMMIT"
};

bool
Scoreboard::findIndex(const RegId& reg, Index &scoreboard_index)
{
    bool ret = false;

    switch (reg.classValue()) {
      case IntRegClass:
        scoreboard_index = reg.index();
        ret = true;
        break;
      case FloatRegClass:
        scoreboard_index = floatRegOffset + reg.index();
        ret = true;
        break;
      case VecRegClass:
      case VecElemClass:
        scoreboard_index = vecRegOffset + reg.index();
        ret = true;
        break;
      case VecPredRegClass:
        scoreboard_index = vecPredRegOffset + reg.index();
        ret = true;
        break;
      case CCRegClass:
        scoreboard_index = ccRegOffset + reg.index();
        ret = true;
        break;
      case MiscRegClass:
          /* Don't bother with Misc registers */
        ret = false;
        break;
      case InvalidRegClass:
        ret = false;
        break;
      default:
        panic("Unknown register class: %d", reg.classValue());
    }

    return ret;
}

RegId
Scoreboard::flattenRegIndex(const RegId& reg)
{
    return reg;
    // return cpu.getContext()->flattenRegId(reg);
}

bool
Scoreboard::canPush(){
    return issue_queue.size() < nr_entries;
}

void
Scoreboard::pushInst(Cva6DynInstPtr inst){
    assert(issue_queue.size() < nr_entries);
    issue_queue.push_back(inst);
}


Scoreboard::DestRegState
Scoreboard::getRegState(Cva6DynInstPtr inst_in, RegId reg, RegVal &val){
    Index index;
    if (!findIndex(reg, index)){
        return FREE;
    }


    int pos = -1;
    // Find instruction position in sb
    for (int i = 0; i < issue_queue.size(); i++){
        assert(!issue_queue[i]->isBubble());
        if (issue_queue[i] == inst_in){
            pos = i;
        }
    }
    // dump();
    fatal_if(pos == -1, "Instruction %s not in scoreboard\n", *inst_in);

     /*
     * sbe0: addi x5, x0, 1 <-- commit head
     * sbe1: addi x0, x5, 1
     * sbe2: addi x5, x5, 1                 // depends on sbe0
     * sbe3: addi x5, x5, 1 <-- Issue head  // depends on sbe2
     */
    // From oldest to newest try to find register
    for (int i = pos - 1; i >= 0; i--){
        Cva6DynInstPtr inst = issue_queue[i];
        assert(inst->issue_completed);
        if (!inst->issue_completed){
            continue;
        }
        StaticInstPtr si = inst->staticInst;
        for (uint8_t i = 0; i < si->numDestRegs(); i++) {
            RegId reg_dst = flattenRegIndex(si->destRegIdx(i));
            Index index_dst;
            if (findIndex(reg_dst, index_dst)) {
                if (index_dst == index){ // Hit register
                    //DPRINTF(Cva6Scoreboard, "Match FW %s\n", *inst);
                    if (inst->reg_dst_val_valid[i]){
                        val = inst->getDstRegOperand(i);
                        // DPRINTF(Cva6Scoreboard, "Hit FW ready %s\n", *inst);
                        return FWABLE;
                    } else {
                        // DPRINTF(Cva6Scoreboard, "Hit FW busy %s\n", *inst);
                        return IN_USE;
                    }
                }
            }
        }
    }
    return FREE;
}

bool
Scoreboard::canInstIssue(Cva6DynInstPtr inst) {

    /* Fault does not have register dependancies */
    if (inst->isFault())
        return true;

    StaticInstPtr staticInst = inst->staticInst;


    /* Available source registers */
    // RaW dependencies
    uint8_t num_srcs = staticInst->numSrcRegs();
    for (uint8_t src_index = 0; src_index < num_srcs; src_index++)
    {
        RegId reg = flattenRegIndex(staticInst->srcRegIdx(src_index));
        RegVal fwval;
        // inst->vp_data.addr_taken = false;
        switch(getRegState(inst, reg, fwval)){
            case IN_USE: {
                /* Load Address prediction */
                // if (inst->vp_data.isPredAddr()){
                //     fwval = inst->vp_data.getPredAddr();
                //     inst->vp_data.addr_taken = true;
                //     DPRINTF(Cva6Scoreboard, "RaW @P reg %s %lx\n",
                    //  reg, fwval);
                // } else {
                DPRINTF(Cva6Scoreboard, "RaW reg %s is already used\n", reg);
                return false;
                // }
            } break;
            case FWABLE: {
                DPRINTF(Cva6Scoreboard, "RaW FW reg %s %lx\n", reg, fwval);
                inst->setSrcRegOperand(src_index, fwval);
            } break;
            case COMMIT:
            case FREE: {
                if (reg.is(InvalidRegClass)) {
                    fwval = 0;
                } else {
                    fwval = cpu.thread->getReg(reg);
                }
                DPRINTF(Cva6Scoreboard, "RaW RR reg %s %lx\n", reg, fwval);
            } break;
        }
        inst->setSrcRegOperand(src_index, fwval);
    }


    /* Available destination registers */
    // WaW dependencies
    const bool enable_waw = false;
    if (enable_waw){
        for (uint8_t i = 0; i < staticInst->numDestRegs(); i++) {
            RegId reg = flattenRegIndex(staticInst->destRegIdx(i));
            RegVal fwval;
            if (getRegState(inst, reg, fwval) != FREE){
                DPRINTF(Cva6Scoreboard, "WaW reg %s is already used\n", reg);
                return false;
            }
        }
    }

    // WaR dependencies
    // Nothing to do

    // RaR dependencies
    // Nothing to do

    return true;
}

/**
 * @brief Forward a register. If register is in the scoreboard it must
 * be forwardable.
 *
 * @param reg. The target register
 * @param val. The returned value
 * @return true when register is in the scoreboard.
 * @return false when register is not in the scoreborad.
 */
bool
Scoreboard::forward(Cva6DynInstPtr inst, RegId reg, RegVal &val){
    return getRegState(inst, reg, val) == FWABLE;
}

void
Scoreboard::issueInst(Cva6DynInstPtr inst, SimpleThread &thread){

    // if (inst->isFault()){
    //     // In case of fault there is no reg deps
    // } else {
    //     /** Forward src registers */
    //     StaticInstPtr si = inst->staticInst;
    //     uint8_t num_src = si->numSrcRegs();
    //     for (uint8_t i_src = 0; i_src < num_src; i_src++) {
    //         /* Get latest register value */
    //         RegId reg = flattenRegIndex(si->srcRegIdx(i_src));
    //         // DPRINTF(Cva6Scoreboard, "setup reg %s\n", reg);
    //         RegVal regv;
    //         if (inst->vp_data.isPredAddr()){
    //             assert(inst->staticInst->isLoad());
    //             regv = inst->vp_data.getPredAddr(); // base addr
    //         } else {
    //             if (forward(inst, reg, regv)) { // Forwarding
    //                 DPRINTF(Cva6Scoreboard, "FW reg %s %x\n", reg, regv);
    //             } else { // Read Register
    //                 if (reg.is(InvalidRegClass)) {
    //                     regv = 0;
    //                 } else {
    //                     regv = thread.getReg(reg);
    //                 }
    //                 DPRINTF(Cva6Scoreboard, "RR reg %s %x\n", reg, regv);
    //             }
    //         }
    //         /* Set src register */
    //         inst->setSrcRegOperand(i_src, regv);
    //     }
    // }if (inst->vp_data.isPredAddr()){
    //     ret = true;
    // }


    /** Finally notify instruction is issued */
    inst->issue_completed = true;
}

Cva6DynInstPtr
Scoreboard::getIssueInst(
    size_t index,
    bool &is_over_serialise,
    bool &is_ready
){
    Cva6DynInstPtr inst = Cva6DynInst::bubble();
    is_over_serialise = false;
    is_ready = false;
    for (Cva6DynInstPtr dyn: issue_queue){
        if (!dyn->issue_completed){
            inst = dyn;
            break;
        }
        if (dyn->isFault() ||
            dyn->staticInst->isSerializeAfter()){
            is_over_serialise = true;
        }
    }
    if (inst->isBubble()){
        return inst;
    }
    is_ready = canInstIssue(inst);
    return inst;
}

void
Scoreboard::completeInst(Cva6DynInstPtr inst){
    // Forward operands
    assert(!inst->execute_completed); // not already commplete
    inst->execute_completed = true; // Finished execution
}

Cva6DynInstPtr
Scoreboard::getCommitInst(size_t index){
    if (index >= issue_queue.size()){
        // No more instruction to commit 1 >= 1
        DPRINTF(Cva6Scoreboard, "commit stall: no instruction\n");
        return Cva6DynInst::bubble();
    }

    Cva6DynInstPtr inst = issue_queue[index];
    if (!inst->execute_completed){
        DPRINTF(Cva6Scoreboard, "commit stall: not executed %s \n", *inst);
        return Cva6DynInst::bubble();
    }
    return inst;
}

void
Scoreboard::commitInst(Cva6DynInstPtr inst){
    // Simply marks instruction
    assert(!inst->commit_completed); // Already commited
    inst->commit_completed = true;
}

void
Scoreboard::tick(){
    dump();
    /* For all instructions to commit */
    while (!issue_queue.empty() &&
          issue_queue.front()->commit_completed) {
        /* Remove instruction from scoreboard*/
        /* Release registers & pop issue queue*/
        Cva6DynInstPtr inst = issue_queue.front();
        issue_queue.pop_front();
    }
}

void
Scoreboard::flush(){
    DPRINTF(Cva6Scoreboard, "flush\n");
    // Flush issue queue
    issue_queue.clear();
}

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
    RegId reg_error = flattenRegIndex(inst_error->staticInst->destRegIdx(0));

    /* Find instruction error position */
    size_t inst_index = -1;
    for (int i = 0; i < issue_queue.size(); i++){
        if (issue_queue[i] == inst_error){
            inst_index = i;
            break;
        }
    }
    fatal_if(inst_index == -1, "instruction not in scoreboard\n");

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

void
Scoreboard::dump(){
    int i = 0;
    char hit[2] = {' ', 'x'};
    for (Cva6DynInstPtr inst: issue_queue){
        if (!inst->isBubble()){
            DPRINTF(Cva6Scoreboard, "sbe#%d [%c][%c][%c] %s\n",
            i++, hit[inst->issue_completed], hit[inst->execute_completed],
            hit[inst->commit_completed], *inst);
        }
    }
}

} // namespace cva6
} // namespace gem5
