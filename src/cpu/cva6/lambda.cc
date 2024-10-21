/**
 * lambda.cc
 *
 *  Author:   Pierre Ravenel
 * Created:   21/09/2024
 **/
#include "cpu/cva6/lambda.hh"

#include "base/named.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/lambda_utils.hh"
#include "cpu/cva6/registerfile.hh"
#include "debug/Cva6LambdaDump.hh"
#include "debug/Cva6LambdaLearn.hh"

namespace gem5 {
namespace cva6 {


#define RST  "\x1B[0m"
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define KBLU  "\x1B[34m"
#define KMAG  "\x1B[35m"
#define KCYN  "\x1B[36m"
#define KWHT  "\x1B[37m"

#ifdef XXX
#define PIPE_START "\xCB"
#define PIPE_END "\xCA"
#define PIPE "\xBA"
#define PIPENO "."
#else
#define PIPE_START "S"
#define PIPE_END "E"
#define PIPE "|"
#define PIPENO "."
#endif

#if 0
std::string pge(uint64_t a, uint64_t b, bool hex=true){
  std::ostringstream os;
  if (hex){
    os << std::hex;
  }
  os << '[' << (a >= b ? KGRN : KRED) << a << ">=" << b << RST << ']';
  return os.str();
}

std::string pe(uint64_t a, uint64_t b, bool hex=true){
  std::ostringstream os;
  if (hex){
    os << std::hex;
  }
  os << '[' << (a == b ? KGRN : KRED) << a << "==" << b << RST << ']';
  return os.str();
}

#endif
void inst_lambda_dump(Cva6DynInstPtr inst){
  static BinaryLambdaRegFile rfd;
  std::ostringstream os;
  static char b2c[2] = {'x', '-'};
  os << "*** ";
  if (inst->l_data.is_predicted_first){
    os << KCYN PIPE_START " " RST;
  } else if (inst->l_data.is_predicted_last){
    os << KCYN PIPE_END " " RST;
  } else if (inst->l_data.is_predicted){
    os << KCYN PIPE " " RST;
  } else {
    os << PIPENO " " ;
  }

  if (inst->l_data.is_learned_first){
    os << KYEL PIPE_START " " RST;
  } else if (inst->l_data.is_learned_last){
    os << KYEL PIPE_END " " RST;
  } else if (inst->l_data.is_learned){
    os << KYEL PIPE " " RST;
  } else {
    os << PIPENO " ";
  }

  if (inst->l_data.is_in_trace_region == 1){
    os << KMAG PIPE_START " " RST;
  } else if (inst->l_data.is_in_trace_region == -1){
    os << KMAG PIPE_END " " RST;
  } else if (inst->l_data.is_in_trace_region){
    os << KMAG PIPE " " RST;
  } else {
    os << PIPENO " ";
  }

  os << b2c[inst->exec_data.is_const_load]
     << b2c[inst->exec_data.is_silent_store]
     << b2c[inst->l_data.is_const];

  // os << '[' << inst->l_data.is_in_trace_region -1 << "] : ";

  os << " " << *inst;
  /* When prediction is made */
  if (inst->l_data.is_predicted_first){
    os << " PRED " << inst->l_data.lambda.str();
  }
  /* When prediction is validated */
  if (inst->l_data.is_predicted_last){
    os << " VALD";
    os << " pc[[" << (inst->l_data.is_check_pc ? KGRN : KRED)
       << inst->l_data.check_pc_next << "=="
       << inst->l_data.lambda.pc_end_next << RST << "[[";
    os << " rd[" << (inst->l_data.is_check_val ? KGRN : KRED)
       << inst->l_data.check_val << "=="
       << inst->l_data.lambda.rd_val << RST << ']';
    os << " cpt[" << (inst->l_data.is_check_indempotance ? KGRN : KRED)
       << inst->l_data.check_cpt_indempotance << ">="
       << inst->l_data.lambda.size << RST << ']';
    os << " K1[";
    if (inst->l_data.is_check_regalloc){
      os << KGRN << "true";
    } else {
      os << KRED << "false";
    }
    os << RST "]";
  }
  /* Learning */
  if (inst->l_data.is_learned_first){
    rfd.clear();
  }
  if (inst->l_data.is_learned){
    rfd.push(inst);
    os << ' ' << rfd.dump();
  }
  /* Learning reg dead */
  if (!inst->isFault()){
    for (unsigned int i = 0; i < inst->staticInst->numSrcRegs(); i++) {
      RegId reg = inst->staticInst->srcRegIdx(i);
      os << ' ';
      os << riscvRegisterName(reg) << ':';
      if (inst->exec_data.is_reg_dead[i]){
        os << "X";
      } else {
        os << '.';
      }
    }
  }
  /* When commit end */
  if (inst->l_data.is_learned_last){
    lambdakto1_t &lambda = inst->l_data.lambdalearn;
    os << " PUSH [" << /* lambdaBtb[lambda] */ 0 << "]" << lambda.str();
  }
  DPRINTF(Cva6LambdaLearn, "%s\n", os.str());
}


bool isInstLamdable(Cva6DynInstPtr inst){
  return !inst->isFault() &&
         !inst->staticInst->isNonSpeculative() &&  /* CSR */
         /* No CSR, break, *fence*, ecall, wfi */
         inst->staticInst->opClass() != No_OpClass &&
         /* No atomics */
         !inst->staticInst->isAtomic() &&
         !inst->staticInst->isStoreConditional() &&
         // inst->exec_data.is_const_load &&
         /* Const load is not mandatory as K->1 Produce the same result */
         inst->exec_data.is_silent_store;
}

bool
LambdaLVTConst::check_and_insert(Cva6DynInstPtr inst){
  uint64_t key = inst->pc->instAddr();
  /* Perfom check */
  uint64_t rdval = 0; /* Default is NULL value */
  if (!inst->isFault()){
    /* (1) For control isntruction de no copy rd but the pc next.
      * Jalr destination register is not really and output of the lambda.
      * (2) Copy the destination register of the instruction. */
    if (inst->staticInst->isControl()){
      rdval = inst->pc_next->instAddr();
    } else if (inst->numDstRegs()){ /* */
      RegId reg = inst->dstRegIdx(0);
      if (reg.classValue() != InvalidRegClass){ /* No x0 */
        rdval = inst->getDstRegOperand(0);
      }
    }
    if (inst->numDstRegs() == 2){
      std::cout << "H2H2 " << *inst << std::endl;
    }
  }
  bool ret = lvt[key] == rdval;
  /* Perform update */
  lvt[key] = rdval;
  return ret;
}

lambdakto1_t
LambdaAlgoLLT::predict(uint64_t pc){
  if (llt.count(pc) && llt[pc].conf.valid()){
    return llt[pc].lambda;
  }
  return { 0 };
}
void
LambdaAlgoLLT::evict(uint64_t pc){
  llt[pc].conf.invalidate();
}
void
LambdaAlgoLLT::commit(Cva6DynInstPtr inst){

  // /* Update table counters */
  // TODO
  // if (inst->l_data.is_valid()){
  //   llt[pc]->inc();
  // } else {
  //   /* Even when miss predictions: update the new value */
  //   bool is_lambda_still_valid = inst->l_data.is_check_indempotance &&
  //                               inst->l_data.is_check_regalloc;
  //   e->dec();
  // }


  /* Learn */
  /* Annotate if instruction is const */
  inst->l_data.is_const = lvt.check_and_insert(inst);
  if (!isInstLamdable(inst) || !inst->l_data.is_const) {
    pushLambda();
    inst_lambda_dump(inst);
  } else {
    window.push_back(inst);
  }
}

void
LambdaAlgoLLT::pushLambda(){
  if (window.size() == 0){
    return;
  }
  /* Compute best score in window */
  uint64_t best_start = 0;
  uint64_t best_delta = 0;
  for (uint64_t start = 0; start < window.size(); start++){
    /* Compute best end for iteration start */
    uint64_t iter_best_end = start;
    BinaryLambdaRegFile rf;
    for (int end = start; end < window.size(); end++){
      rf.push(window[end]);
      if (rf.isKto1()){
        iter_best_end = end;
      }
    }
    uint64_t iter_best_delta = iter_best_end - start + 1;
    /* Is better */
    if (iter_best_delta > best_delta) {
      best_start = start;
      best_delta = iter_best_delta;
    }
  }
  uint64_t best_end = best_start+best_delta-1;
  // DPRINTF(Cva6LambdaLearn,
  //   "*Best Score [[%d [%d:%d] %d[[ : %d/%d\n",
  //   0, best_start, best_end, window.size(),
  //   best_delta, window.size());
  assert(best_end < window.size());

  /* Prunning */
  if (best_delta == 1){ /* */
    if (!window[best_start]->staticInst->isLoad()){
      best_delta = 0;
    }
  }
  /* Push lambda if found one */
  if (best_delta){
    /* Compute Window to get register rd value */
    static LambdaRegFile rf(cpu);   /* Compute real register file */
    rf.clear();
    // DPRINTF(Cva6LambdaLearn, "*** learn on:\n");
    for (int i = best_start; i <= best_end; i++){
      rf.push(window[i]);
    }
    fatal_if(!rf.isKto1(), "Must be k->1\n");
    std::pair<RegId, uint64_t> pair =
      rf.isRdSingle() ? rf.getSingle()
      : std::pair<RegId, uint64_t>(RiscvISA::intRegClass[0], 0); // Always true
    uint64_t id = id2i(pair.first);

    /* Create Lambda */
    lambdakto1_t lambda = {
      .pc_start = window[best_start]->pc->instAddr(),
      .pc_end = window[best_end]->pc->instAddr(),
      .pc_end_next = window[best_end]->pc_next->instAddr(),
      .size = best_delta,
      .rd = id,
      .rd_val = pair.second
    };

    /* Update metadata */
    for (int i = 0; i < window.size(); i++){
      bool inWindow = i >= best_start && i <= best_end;
      if (inWindow){
        window[i]->l_data.is_learned_first = i == best_start;
        window[i]->l_data.is_learned_last = i == best_end;
        window[i]->l_data.is_learned = inWindow;
        window[i]->l_data.lambdalearn = lambda;
      }
      window[i]->l_data.is_in_trace_region = i+1;
    }
    window[window.size()-1]->l_data.is_in_trace_region = -1;

    /* Insert lambda in LLT */
    llt[lambda.pc_start] = LLTEntry_t(lambda);

    // Append Lambda for statistics
    if (lambdaBtb.count(lambda) > 0){
      stats.replayL += lambda.size;
      stats.Lsize.sample(lambda.size);
    }
    lambdaBtb[lambda] += 1; // Increment
    int score =  lambdaBtb[lambda] * lambda.size;
    if (saves.count(lambda)){ /* Simply update */
      /* Check instruction flows */
      std::vector<Cva6DynInstPtr> &vec = *saves[lambda];
      if (0){
        for (int i = 0; i < lambda.size; i++){
          Cva6DynInstPtr inew = window[best_start + i];
          Cva6DynInstPtr iold = vec[i];
          DPRINTF(Cva6LambdaLearn, "%d == %s ?\n", *inew, *iold);
          // assert(state_t(inew) == state_t(iold)); // Compare inst states
          /* Not True
          * Load may read same values at different address.
          *
          */
        }
      }
      /* Set
        minscore = score;new score */
      savesscores[lambda] = score;
      if (score > minscore){
      }
    } else { /* */
      bool need_update = false;
      if (savesscores.size() < PQSIZE) {
        need_update = true;
      } else if (score > minscore) {
        need_update = true;
        auto it = std::min_element(
                    std::begin(savesscores),
                    std::end(savesscores),
                    [](const auto& l, const auto& r) {
                      return l.second < r.second; });
        delete saves[it->first]; // Delete vector
        saves.erase(it->first); // erase vector entry
        savesscores.erase(it->first); // erase score entry
      }
      if (need_update){
        savesscores[lambda] = score;
        if (score > minscore){
          minscore = score;
        }
        /* Copy insts */
        auto vec = new std::vector<Cva6DynInstPtr>(
          window.begin() + best_start,
          window.begin() + best_end + 1);
        /* Insert vector */
        saves[lambda] = vec;
      }
    }
  }

  // DPRINTF(Cva6LambdaLearn, "push Lambda [%d]: %s\n",
  //             lambdaBtb[lambda], lambda.str());

  /* Finally dump delayed display */
  for (int i = 0; i < window.size(); i++){
    inst_lambda_dump(window[i]);
  }
  window.clear();
}

void
LambdaAlgoLLT::dump(){
  for (auto e: saves){
    const lambdakto1_t &lambda = e.first;
    std::vector<Cva6DynInstPtr> &vec = *e.second;
    assert(savesscores.count(lambda));
    int score = savesscores[lambda];
    std::cout << " * Best lambda is" <<  lambda.str();
    std::cout << " replay = " << score/lambda.size;
    std::cout << " score = " << score << std::endl;
    for (auto &e: vec){
      std::cout << *e << std::endl;
    }
  }
}

void
LambdaHandler::on_fetch(Cva6DynInstPtr inst){
  if (!lltSize){
    return;
  }
  uint64_t pc = inst->pc->instAddr();
  if (!in_lambda){ /* Try to perform prediction */
    prediction = algo.predict(pc);
    predictionttl = prediction.size-1;
    in_lambda = prediction.size != 0;
  }
  /* Annotate predicted instructions */
  if (in_lambda){
    inst->l_data.is_predicted_first = predictionttl == prediction.size-1;
    inst->l_data.is_predicted_last = predictionttl == 0;
    inst->l_data.is_predicted = true;
    inst->l_data.lambda = prediction;
  }
  if (inst->l_data.is_predicted_last){
    in_lambda = false;
  }
  /* tick */
  if (predictionttl != -1) {
    predictionttl -= 1;
  }

  /* (1) : maintain reg allog valid (can be performed at fetch) */
  if (inst->l_data.is_predicted_first){
    rf.clear();
  }
  if (inst->l_data.is_predicted){
    rf.push(inst);
  }
  /* Somewehere between fetch and commit */
  if (inst->l_data.is_predicted_last){/* If we reach end of lambda */
    inst->l_data.do_ckeck_k1(rf.isKto1());
  }
}

void
LambdaHandler::on_noisy_store(Cva6DynInstPtr inst){
  if (!lltSize){
    return;
  }
  assert(inst->l_data.is_predicted);
  uint64_t bad_pc = inst->l_data.lambda.pc_start;
  algo.evict(bad_pc);
}

/*
 * Things to validate:
 * at fetch:
 *  1) . pc end (not mandatory ?) Only if last instruction is branch
 *  2) * is K->1
 * at execute:
 *  3) * No store that change memory state with indempotance counter
 *  4) * Predicted value
 *     . Not constant load ?
 * */
bool
LambdaHandler::on_commit(Cva6DynInstPtr inst){
  if (!lltSize){
    return true;
  }
  bool valid = true;
  /* (0) : maintain indempotence counter */
  bool isLambdable = isInstLamdable(inst);
  if (isLambdable){
    last_landable_cpt += 1;
  } else {
    last_landable_cpt = 0;
  }

  /* */
  if (inst->l_data.is_predicted_last){ /* Check PC end */
    inst->l_data.do_check_pc_next(inst->pc_next->instAddr());
  }
  /* Commit validation */
  if (inst->l_data.is_predicted_last){
    RegId reg = i2id(inst->l_data.lambda.rd);
    uint64_t value = cpu.thread->getReg(reg);
    valid = inst->l_data.do_final_check(value, last_landable_cpt);
    stats.req += 1;
    stats.hit += valid;
    stats.miss += !valid;
    stats.miss_indempotance += !inst->l_data.is_check_indempotance;
    stats.miss_val += !inst->l_data.is_check_val;
    stats.miss_pc += !inst->l_data.is_check_pc;
    if (valid){
      stats.hitLsize.sample(inst->l_data.lambda.size);
    }
    if (!valid){
      // Drop prediction
      assert(inst->l_data.is_predicted);
      uint64_t bad_pc = inst->l_data.lambda.pc_start;
      algo.evict(bad_pc);
    }
  }
  return valid;
}

void
LambdaHandler::on_post_commit(Cva6DynInstPtr inst){
  /* Learn new lambdas ... */
  algo.commit(inst);
}


Cva6DynInstPtr
LambdaHandler::newPredInst(Cva6DynInstPtr inst){
  assert(inst->l_data.is_predicted);
  lambdakto1_t p = inst->l_data.lambda;
  StaticInstPtr si = new LambdaPredInst(i2id(p.rd), p.rd_val);
  assert(si);
  /* Create the compound Dynamic instruction */
  Cva6DynInstPtr i2 = new Cva6DynInst(&cpu, si, &inst->pc);
  /* Increment sequence number */
  i2->id.fetchSeqNum = inst->id.fetchSeqNum;
  i2->id.uop_extra = inst->id.uop_extra + 1;
  assert(i2->staticInst);
  /* Annotate i2 */
  i2->l_data.is_uop_lambda_pred = true;
  return i2;
}

} // namespace cva6
} // namespace gem5
