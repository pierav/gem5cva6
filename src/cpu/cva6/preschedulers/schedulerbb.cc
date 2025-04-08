#include "cpu/cva6/preschedulers/schedulerbb.hh"

namespace gem5 {
namespace cva6 {

void
SchedulerBB::push(Cva6DynInstPtr inst) {
  tempoq.push_back(inst);
  if (wait_drain && bbq.front()->empty()){ // Drain resolved, fill new slot
    bbq.push_back(bbq.front()); // Move last one to cur
    bbq.pop_front();
    wait_drain = false;
  }
  /* Try to fill BB with temp data */
  while (!tempoq.empty() && !wait_drain){
    inst = tempoq.front();
    tempoq.pop_front();
    wait_drain = !inst->isFault() && inst->staticInst->isControl();
    // DPRINTF(Cva6Sched, "Push %s\n", *inst);
    bbq.back()->push(inst);
  }
}

Cva6DynInstPtr
SchedulerBB::pop() {
  Cva6DynInstPtr ret = internal_pop();
  /* Annotate instruction delta */
  // TODO
  ret->delta = mainsa.commit(ret);
  return ret;
}

Cva6DynInstPtr
SchedulerBB::internal_pop(){
  /* Try OoO schedule */
  /* Compute instruction ready in respect to WaW deps */
  bool ready_to_schedule[NBBBQ];
  for (int i = 0; i < NBBBQ; i++){
    ready_to_schedule[i] = false;
    if (bbq[i]->empty()){
      continue;
    }
    Cva6DynInstPtr inst = bbq[i]->front();
    ready_to_schedule[i] = true;
    for (int j = 0; j < i; j++){
      if (bbq[j]->isDependancy(inst)){
        ready_to_schedule[i] = false;
        break;
      }
    }
  }
  int ready_to_schedule_cnt = 0;
  for (int i = 0; i< NBBBQ; i++){
    ready_to_schedule_cnt += ready_to_schedule[i];
  }

  uint64_t deltas[NBBBQ];
  for (int i = 0; i < NBBBQ; i++){
    if (!bbq[i]->empty()){
      deltas[i] = mainsa.getInstReadyDeltaTime(bbq[i]->front());
    }
  }

  assert(ready_to_schedule_cnt);
  DPRINTF(Cva6Sched, "============= schedpop() =============\n");
  if (ready_to_schedule_cnt){ /* Issue instruction with min Latency */
    int best_delta_idx = 0;
    int best_delta = 1000000; // Huge score
    for (int i = NBBBQ-1; i >= 0; i--){
      // Backward to prioritize recent instructions
      if (ready_to_schedule[i] && (deltas[i] <= best_delta)){
        best_delta_idx = i;
        best_delta = deltas[i];
      }
    }

    /* DUMP */
    for (int i = 0; i < NBBBQ; i++){
      DPRINTF(Cva6Sched, "BBQ[%d](ready=%d, delta=%d) :: [%s]\n",
        i, ready_to_schedule[i], deltas[i],
        i == best_delta_idx ? "HIT" : "");
      for (int j = 0; j < bbq[i]->holdqueue.size(); j++){
        DPRINTF(Cva6Sched, "BBQ[%d](%d): %s\n", i, j,
          dumpInstPreg(bbq[i]->holdqueue[j]));
      }
    }

    stats.req += 1;
    stats.rescheduled += best_delta_idx != 0;
    return bbq[best_delta_idx]->pop();
  } else { /* Issue pending instruction */
    assert(!tempoq.empty());
    auto ret = tempoq.front();
    tempoq.pop_front();
    return ret;
  }
}


} // namespace cva6
} // namespace gem5
