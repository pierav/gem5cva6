/**
 * vp_dpe.cc
 *
 *  Author:   Pierre Ravenel
 * Created:   04/03/2024
 **/

#include "cpu/cva6/vp_dpe.hh"

#include <iomanip>

#include "base/named.hh"
#include "cpu/cva6/pipeline.hh"
#include "cpu/cva6/vp.hh"
#include "debug/Cva6VP.hh"

namespace gem5 {
namespace cva6 {

void
VPDPE::predict(Cva6DynInstPtr inst){
  if (vp.isEnable()){
    if (!inst->vp_data.is_predicted){
      ghist.dump();
      vp.predict(&inst->vp_data);
      inst->vp_data.is_predicted = true;
    } else {
      /* overpredict if needed */
      if (is_fresh_commited_values){
        ghist.dump();
        vp.predict(&inst->vp_data);
      }
      // printf("Overpredict\n");
    }
  }
  /* In all cases updates predict timestamp for stats */
  inst->vp_data.time_predict = cpu.curCycle();
  /* Predict L1 hit/miss */
  inst->vp_data.hmp_l1hit_pred = hmp.predict(inst);
  // DPRINTF(Cva6VP, "predict %s at %d\n", *inst, inst->vp_data.time_predict);
  /* Predict memory dependancies */
  cpu.pipeline->mdp.checkInst(inst->pc->instAddr(), &inst->mdpinst);
}

void
VPDPE::insert(Cva6DynInstPtr inst){
  vp.insert(&inst->vp_data, ghist);
  // Compute inst context
  inst->vp_data.seqNum = inst->id.fetchSeqNum;
  ghist.insert(inst);

  /* Mdp things */
  if (!inst->isFault() && inst->staticInst->isStore()){
    cpu.pipeline->mdp.pushStore(inst->pc->instAddr(), inst);
  }

  inflights.push_back(inst);
  // Compute static data
  if (!inst->isFault() &&        /** Not a adress fault */
      inst->staticInst->isLoad() /** Only predict memory load */
  ){
    inst->executeInitiateStatic();
    inst->vp_data.inst_mem_req_imm = inst->static_data.mem_req_imm;
    inst->vp_data.inst_mem_req_size = inst->static_data.mem_req_size;
    inst->vp_data._pc = inst->pc->instAddr();
    if (DPE_IGNORE){
      /* Perform prediction only in the fetch stage */
      predict(inst);
      if (TEST_MODE){ // Clear predictions
        inst->vp_data.addr_ready = false;
        inst->vp_data.value_ready = false;
      }
      /* By default take prediction */
      inst->vp_data.value_taken = inst->vp_data.value_ready;
    }
  }
}

bool
VPDPE::vp_perform_issue(Cva6DynInstPtr inst){
  // Perfom VP
  bool need_execution = true;
  if (vp.isEnable() &&
    !inst->isFault() &&
    inst->staticInst->isLoad()){
    fatal_if(!inst->vp_data.is_predicted,
      "Inst %d is not predicted\n", *inst);
    inst->vp_data.time_issue = cpu.curCycle();
    // int64_t delta = inst->vp_data.time_issue -
    //       inst->vp_data.time_predict;
    // TODO predict at fetch
    // fatal_if(delta < VP_DELAY, "Timing anomaly : delta = %d\n", delta);
    assert(inst->dreq);
    uint64_t base_addr = inst->getSrcRegOperand(0);
    uint64_t eff_addr = inst->dreq->req->getVaddr();
    assert(eff_addr == base_addr + inst->static_data.mem_req_imm);
    bool taken = vp.update_prediction_at_issue(&inst->vp_data,
        base_addr);
    if (TEST_MODE){ // Force the effetive execution
      inst->vp_data.hit = false;
    }
    if (taken){
      need_execution = !inst->vp_data.hit;
      fatal_if(inst->staticInst->numDestRegs() != 1,
          "VP: si->numDestRegs() must be equal to 1.\n");
      // Notify scoreboard that destination register is ready
      // with the predicted value
      // We do this shitty flow to perform sign extension
      inst->dreq->complete_forward(inst->vp_data.pred_val);
      inst->executeComplete();
      // Reset inst if non deterministic TODO
      if (need_execution){
          inst->untrackDreq();
          inst->setFaultEx(NoFault);
          inst->executeInitiate();
      }
      // inst->setDstRegOperand(0, inst->vp_data.pred_val);
    }
  }

  // WB store
  if (vp.isEnable() &&
  !inst->isFault() &&
  (inst->staticInst->isStore() ||
    inst->staticInst->isAtomic())){

    // uint64_t base_addr = inst->getSrcRegOperand(0);
    uint64_t eff_addr = inst->dreq->req->getVaddr();
    uint64_t pc = inst->pc->instAddr();
    vp.store_issued(pc, eff_addr);

    // First invalidate inflight instructions
    uint64_t eff_tag = eff_addr >> 3;
    for (Cva6DynInstPtr ifinst: inflights){
      vp_inst_metadata_t *res = &ifinst->vp_data;
      uint64_t res_eff_tag = (res->t1_addr + res->inst_mem_req_imm) >> 3;
      if (res_eff_tag == eff_tag){
        res->pred_val_vastra_valid = false;
      }
    }
  }
  return need_execution;
}


/**  Make prediction int the 2 DeltaCycle Window */
void
VPDPE::perform_window_predictions(){
  /* Remove issued instructions */
  while (!inflights.empty() && inflights.front()->issue_completed){
    issued.push_back(inflights.front());
    inflights.pop_front();
  }

  if (DPE_IGNORE){/* Ignore prediction window */
    return;
  }

  int start = -1;

  /* Compute prediction window start */
  for (int i = 0; i < inflights.size(); i++){ // 4 first insts
    int available_delay = VP_DELAY - (i / ISSUE_WIDTH);
    if (inflights[i]->cycle_from_issue() >= available_delay){
      start = i;
      break;
    }
  }

  /* Perform predictions */
  if (start != -1){
    int end = std::min(start + ISSUE_WIDTH, (int)inflights.size());
    for (int i = start; i < end; i++){
      Cva6DynInstPtr inst = inflights[i];
      // Compute prediction if necessary
      if (vp.isEnable() &&
        !inst->isFault() &&
        inst->staticInst->isLoad())
      {
        predict(inst);
      }
    }
    is_fresh_commited_values = false; // All predictions were made
  }

}

bool
VPDPE::issue(Cva6DynInstPtr inst){
  // Pop issued instructions
  // assert(!inflights.empty());
  // if (inflights.front() != inst){
  //   fatal("%s != %s\n", *inflights.front(), *inst);
  // }

  /* Mdp things */
  if (!inst->isFault() && inst->staticInst->isStore()){
    cpu.pipeline->mdp.popStore(inst->pc->instAddr(), inst);
  }

  // Update Predictor
  // bool need_execution = vp_perform_issue(inst);
  bool need_execution = true;
  // Make prediction if needed
  // update_prediction_window();
  return need_execution;
}

bool
VPDPE::post_commit(Cva6DynInstPtr inst){
  /* Annotate effective base address */
  if (inst->vp_data.is_predicted){
    StaticInstPtr si = inst->staticInst;
    assert(si->numSrcRegs() == 1);
    RegId reg = si->srcRegIdx(0);
    inst->vp_data.eff_addr = cpu.thread->getReg(reg);
  }
  stats.req += 1;
  /* Check missprediction */
  if (inst->vp_data.addr_taken){
    assert(inst->staticInst);
    assert(inst->staticInst->isLoad());
    // Post commit
    Addr pc = inst->pc->instAddr();
    bool baseaddr_match = inst->vp_data.eff_addr == inst->vp_data.t1_addr;

    DPRINTF(Cva6VP, "%16lx: VP@ : %lx =? %lx :: %d\n",
      pc, inst->vp_data.eff_addr, inst->vp_data.t1_addr, baseaddr_match);

    stats.predaddr_taken += 1;
    stats.predaddr_takenhit += baseaddr_match;

    // static int hitsa[2];
    // hitsa[baseaddr_match] += 1;
    // DPRINTF(Cva6VP, "VP@ : #(t^@)=%d, #(t^!@)=%d :: %f\n",
    //     hitsa[1], hitsa[0], (float)hitsa[1]/(hitsa[1] + hitsa[0]));

    bool misspred = !baseaddr_match;
    if (misspred){
      // Clear AP entry
      // vp.getAP().commit(pc, inst->vp_data.eff_addr, &inst->vp_data);
      vp.getAP()->update_conf(pc, false, &inst->vp_data);
    }
    return misspred;
  }
  return false;
}

/* TODO PERFORM INFLIGHT REPLAY! */
bool
VPDPE::commit(Cva6DynInstPtr inst){
    assert(issued.front() == inst);
    issued.pop_front();
    if (vp.isEnable() &&
      !inst->isFault() &&
      inst->staticInst->isMemRef()
    ){
      assert(inst->dreq);
      Addr pc = inst->pc->instAddr();
      bool isload = inst->staticInst->isLoad();
      bool isamo = inst->staticInst->isAtomic();
      if (!inst->vp_data.hit) { // Need update
          // TODO: do not flush when unbufferable
          if (isload){
              bool is_bufferable = inst->dreq->isCl();
              assert(!isamo);
              if (is_bufferable){
                  vp.fake_cache_write(pc,
                      inst->dreq->getClVaddr(),
                      inst->dreq->getClSize(),
                      inst->dreq->getClData(),
                      isload, false);
              }
          } else {
              bool flush = isamo; // Amo must be executed
              uint64_t data = inst->dreq->getData();
              vp.fake_cache_write(pc,
                  inst->dreq->req->getVaddr(),
                  inst->dreq->req->getSize(),
                  (uint8_t*)&data, isload, flush);
          }
      }
      if (isload){
          uint64_t base_addr = inst->getSrcRegOperand(0);
          uint64_t real_val = inst->getDstRegOperand(0); //
          // CARE SIGN EXTENSION !!!!
          // inst->dreq->getData();
          // inst->getDsrRegOperand(0);
          uint64_t pred_val = inst->vp_data.pred_val;
          inst->vp_data.hit = inst->vp_data.value_taken &&
              (pred_val == real_val);
          // Commit real value
          vp.commit(pc, base_addr, inst->dreq->req->getSize(),
              real_val, &inst->vp_data, isload);
          // Stats
          uint64_t delta = inst->vp_data.time_issue -
            inst->vp_data.time_predict;
          stats.cycles_pred_commit.sample(delta);
      } else { // Store or amo
          uint64_t eff_addr = inst->dreq->req->getVaddr();
          vp.store_commit(pc, eff_addr);
      }
      /* Notifie there is fresh values */
      is_fresh_commited_values = true;
    }

    bool misspred = false;
    if (vp.isEnable() &&
        !inst->isFault() &&
        inst->staticInst->isLoad()
    ){ /* Missprediction taken */
        misspred = inst->vp_data.value_taken && !inst->vp_data.hit;
        fatal_if(TEST_MODE && misspred, "MISSPRED!\n");
    }

    /* History management */
    ghist.commit(inst);

    /* Hit Miss predictor update */
    if (!inst->isFault() && inst->staticInst->isLoad()){
      hmp.commit(inst, inst->vp_data.hmp_l1hit_pred);
    }

    #if 0
    if (!inst->isFault() && inst->staticInst->isLoad()){
      // Predict V_Tage addr
      Addr pc = inst->pc->instAddr();
      // printf("**** PREDICT pc = %lx\n****\n", pc);
      void *history;
      prediction_t pred;

      /* Perfom prediction */
      pred = vtage.lookup(pc, history, inst->id.fetchSeqNum);
      /* Match */
      uint64_t predval = pred.first;
      // Base ADDR
      // uint64_t effval = inst->getSrcRegOperand(0); // The base addr
      // Eff value
      uint64_t effval = inst->getDstRegOperand(0);
      bool misspred = predval != effval;
      //printf("Vtage pred %lx, eff %lx, miss : %d\n",
      //   predval, effval, misspred);
      /* Update */
      vtage.update(effval, history, misspred, false);
      // printf("**** DONE UPDATE ****\n");

    }
    #endif

    return misspred;
}


} // namespace cva6
} // namespace gem5
