/**
 * @file stage_decode.cc
 * @author Pierre Ravenel (pravenel@kalray.eu)
 * @brief
 * @version 0.1
 * @date 2023-05-25
 *
 */

#include "cpu/cva6/stage_decode.hh"

#include "debug/Decode.hh"

namespace gem5 {
namespace cva6 {


void
Decode::evaluate(){
    if (resolved_branch.isStreamChange()) {
       flush();
       return;
    }

    // while (inp.canPop() && out.canPush()) { // In OK and Out OK
    //     bool input_finished = false;
    //     // decode and push
    //     Cva6DynInstPtr inst = decodeInst(inp.front(), input_finished);
    //     out.push(inst);
    //     dpe.insert(inst);
    //     if (input_finished){
    //         inp.pop();
    //     }
    // }

    // A simple R/V flip flip
    while (inp.canPop() && out.canPush()){
        Cva6DynInstPtr inst = inp.pop();
        inst->stage_decode_enter = true;
        out.push(inst);
    }
}

void
Decode::flush(){
    DPRINTF(Decode, "Flush inp\n");
    inp.flush();
    //
}


} // namespace cva6
} // namespace gem5
