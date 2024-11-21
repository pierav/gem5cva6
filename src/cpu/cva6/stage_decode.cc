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

  #define USE_SCHED 0

  if (USE_SCHED){
    // Decode stage : -> [ DECODE ] ->
    while (inp.canPop() && cpu.pipeline->sa.can_push_at_decode()){
      Cva6DynInstPtr inst = inp.pop();
      inst->stage_decode_enter = true;
      cpu.pipeline->sa.rename(inst);
      cpu.pipeline->sa.push_at_decode(inst);
      /* Push also in rob to keep track of isntruction order */
      cpu.pipeline->rob.push(inst);
    }

    // Scheduler stage -> [ SCHED ] ->
    while (cpu.pipeline->sa.can_pop_at_decode() && out.canPush()){
      out.push(cpu.pipeline->sa.pop_at_decode());
    }
  } else {
    while (inp.canPop() && out.canPush()){
      Cva6DynInstPtr inst = inp.pop();
      cpu.pipeline->sa.rename(inst);
      out.push(inst);
      /* Push also in rob to keep track of isntruction order */
      cpu.pipeline->rob.push(inst);
    }
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
