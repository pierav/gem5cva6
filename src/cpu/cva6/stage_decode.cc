/**
 * @file stage_decode.cc
 * @author Pierre Ravenel (pravenel@kalray.eu)
 * @brief
 * @version 0.1
 * @date 2023-05-25
 *
 */

#include "cpu/cva6/stage_decode.hh"
#include "cpu/cva6/pipeline.hh"
#include "debug/Decode.hh"

namespace gem5 {
namespace cva6 {

bool needArchSerialize(Cva6DynInstPtr& inst){
    // TODO : remove this part !
    // There is bad value for load, maybe bad AmoBuff -> LQ
    if (!inst->isFault() &&
        (inst->staticInst->isAtomic() ||
        inst->staticInst->isStoreConditional())){
      return true;
    }

    // TODO : rename fflags and do not squash
    return inst->isFault() /* ITLB failure*/ ||
           inst->staticInst->isSerializing() ||
           inst->staticInst->isSquashAfter() ||
           inst->staticInst->isReadBarrier() ||
           inst->staticInst->isWriteBarrier();
}

void
Decode::evaluate(){
  if (resolved_branch.isStreamChange()) {
    flush();
    return;
  }

  // Decode stage :
  while (inp.canPop() &&
    cpu.pipeline->sa.can_push_scheduler(inp.front()))
  {
    Cva6DynInstPtr inst = inp.pop();
    inst->stage_decode_enter = true;
    /* Annotate instruction flags */
    inst->needArchSerialize = needArchSerialize(inst);
    /* Push in the prediction pipeline */
    cpu.pipeline->dpe.insert(inst);
    /* Push in the scheduler stage */
    cpu.pipeline->sa.push_scheduler(inst);
    /* Push also in rob to keep track of instruction order */
    cpu.pipeline->rob.push(inst);
  }

  cpu.pipeline->dpe.perform_window_predictions();

  // while (inp.canPop() && out.canPush()){
  //   Cva6DynInstPtr inst = inp.pop();
  //   cpu.pipeline->sa.rename(inst);
  //   out.push(inst);
  //   /* Push also in rob to keep track of isntruction order */
  //   cpu.pipeline->rob.push(inst);
  // }
}

void
Decode::flush(){
    DPRINTF(Decode, "Flush inp\n");
    inp.flush();
}


} // namespace cva6
} // namespace gem5
