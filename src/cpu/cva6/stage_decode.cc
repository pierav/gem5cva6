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

void
Decode::evaluate(){
  if (resolved_branch.isStreamChange()) {
    flush();
    return;
  }

  // Decode stage :
  while (inp.canPop() && cpu.pipeline->sa.can_push_scheduler()){
    Cva6DynInstPtr inst = inp.pop();
    inst->stage_decode_enter = true;
    cpu.pipeline->sa.push_scheduler(inst);
    /* Push also in rob to keep track of instruction order */
    cpu.pipeline->rob.push(inst);
  }

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
