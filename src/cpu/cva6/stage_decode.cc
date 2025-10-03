/**
 * @file stage_decode.cc
 * @author Pierre Ravenel (pravenel@kalrayinc.com)
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
  DPRINTF(Decode, "Have %d insts to send\n", inp.size());
  while (inp.canPop() &&
    (inp.front()->ts_fetch_completed + front_latency) < cpu.curCycle() &&
    cpu.pipeline->rob.canPush()
  ) {
    /* !!! do can_push_scheduler at last because perform rename !!! */
    // Check if reg allocation is possible and do it
    if (!cpu.pipeline->sa.can_push_scheduler(inp.front())){
      DPRINTF(Decode, "Can't push schedule %s\n", *inp.front());
      break;
    }
    Cva6DynInstPtr inst = inp.pop();
    DPRINTF(Decode, "Send to next stage %s\n", *inst);
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
  cpu.pipeline->sa.tick(); // Tick the scheduler

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
