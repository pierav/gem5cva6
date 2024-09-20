#include "cpu/cva6/pure_block.hh"

#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>

#include "arch/riscv/utility.hh"
#include "cpu/cva6/lambda_types.hh"
#include "cpu/cva6/lambda_utils.hh"
#include "cpu/cva6/registerfile.hh"
#include "debug/Cva6Commit.hh"
#include "debug/Cva6LambdaCommit.hh"
#include "debug/Cva6LambdaDump.hh"
#include "debug/Cva6LambdaLearn.hh"
#include "debug/Cva6LambdaRDA.hh"

namespace gem5 {
namespace cva6 {

/* Lamdba learn statistics */
static std::map<struct lambdakto1_t, size_t> lambdaBtb;

/* Debug only to track sequence of instructions */
int PQSIZE = 5; // number of indices we need
using LS_t = std::pair<int, lambdakto1_t>; // score lambda
std::priority_queue<LS_t> q;
std::map<lambdakto1_t, std::vector<Cva6DynInstPtr>*> saves;
std::map<lambdakto1_t, int> savesscores;
int minscore = 0;

/* Last Lambda Table */
static std::map<uint64_t, lambdakto1_t> llt;


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

  os << '[' << inst->l_data.is_in_trace_region -1 << "] : ";
  os << *inst;
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
      os << riscvRegisterName(reg) << ':';
      if (inst->exec_data.is_reg_dead[i]){
        os << "X";
      } else {
        os << '.';
      }
      os << ' ';
    }
  }
  /* When commit end */
  if (inst->l_data.is_learned_last){
    lambdakto1_t &lambda = inst->l_data.lambdalearn;
    os << " PUSH [" << lambdaBtb[lambda] << "]" << lambda.str();
  }
  DPRINTF(Cva6LambdaLearn, "%s\n", os.str());
}



void
InstStatesHandler::commit(Cva6DynInstPtr inst){
  // Compute instruction HASH
  // inststate_t inststate(inst);
  /* Keep track of K last committed instructions */
  committed.push_front(inst);
  committed.pop_back();


  const bool enable_suffix_tree = true;
  if (enable_suffix_tree){ /* Suffix pattern match */

    /* Compute instruction unique key */
    const state_t* state = &*memsave.insert(state_t(inst)).first;

    /* Read tree */
    size_t depth = suffixtree.insert(state);
    assert(depth <= NB_INFLIGHT);

    size_t depth_const_load = 0;
    for (int i = 0; i < depth; i++){
      if (committed[i]->exec_data.is_const_load){
        depth_const_load++;
      } else {
        break;
      }
    }

    size_t depth_silent_store = 0;
    for (int i = 0; i < depth; i++){
      if (committed[i]->exec_data.is_silent_store){
        depth_silent_store++;
      } else {
        break;
      }
    }

    BinaryRegisterFile rfsrc;
    BinaryRegisterFile rfdst;
    size_t depth_lambda21_best = 0;
    size_t depth_lambdak1_best = 0;

    for (int i = 0; i < depth; i++){
      Cva6DynInstPtr inst = committed[i];
      // Append all regs dependancdies
      for (unsigned int i = 0; i < inst->staticInst->numSrcRegs(); i++) {
        RegId reg = inst->staticInst->srcRegIdx(i);
        if (reg.classValue() != InvalidRegClass){
          if (!rfdst.isSet(reg)){ /* Is register lambda input */
            rfsrc.set(reg);
          }
          /* Perform reg dead check */
          if (inst->exec_data.is_reg_dead[i]){
            rfdst.clear(reg);
          }
        }
      }
      /* Append all destination registers */
      for (unsigned int i = 0; i < inst->staticInst->numDestRegs(); i++) {
        RegId reg = inst->staticInst->destRegIdx(i);
        if (reg.classValue() != InvalidRegClass){
          rfdst.set(reg);
        }
      }
      /* Evaluate potential lambda */
      if (rfsrc.popcount() <= 2 && rfdst.popcount() <= 1){
        depth_lambda21_best = i+1;
      }
      if (rfdst.popcount() <= 1){
        depth_lambdak1_best = i+1;
      }
    }

    size_t depth_clss = std::min(depth_const_load, depth_silent_store);
    stats.req += 1;
    stats.replay[depth] += 1;
    stats.replay_cl[depth_const_load] += 1;
    stats.replay_ss[depth_silent_store] += 1;
    stats.replay_clss[depth_clss] += 1;

    stats.replay_rd[depth_lambda21_best] += 1;
    stats.replay_rdk1[depth_lambdak1_best] += 1;
    // /* Iterate over suffix */
    // // std::cout << "Path (#" << depth << "):" << std::endl;
    // for (auto &e: suffixtree){
    //   // std::cout << "e: " << *e << std::endl;
    // }
  }
}



void
PureBlock::pushLambda(){
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
    llt[lambda.pc_start] = lambda;

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

bool isInstLamdable(Cva6DynInstPtr inst){
  return !inst->isFault() &&
         !inst->staticInst->isNonSpeculative() &&  /* CSR */
         /* No CSR, break, *fence*, ecall, wfi */
         inst->staticInst->opClass() != No_OpClass &&
         inst->exec_data.is_const_load &&
         inst->exec_data.is_silent_store;
}


class LambdaCheckInst : public StaticInst
{
  uint64_t check;
  RegId srcRegIdxArr[1]; RegId destRegIdxArr[0];
  public:
  LambdaCheckInst(RegId reg, uint64_t v)
    : StaticInst("lambda.check", IntAluOp), check(v) {
    setRegIdxArrays(
      reinterpret_cast<RegIdArrayPtr>(
          &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
      reinterpret_cast<RegIdArrayPtr>(
          &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
    setSrcRegIdx(_numSrcRegs++, reg);
    flags[IsInteger] = true;
    // flags[IsMicroop] = true;
  }
  Fault execute(ExecContext *xc, trace::InstRecord *td) const override {
    uint64_t val = xc->getRegOperand(this, 0);
    if (check != val){
      return std::make_shared<SpeculativeFault>("missprediction");
    }
    return NoFault;
  }
  void advancePC(PCStateBase &pc_state) const override { /* Nothing */}
  std::string
  generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const {
    std::stringstream ss;
    ss << mnemonic << ' ';
    ss << registerName(srcRegIdxArr[0]) << ", ";
    ss << check;
    return ss.str();
  }
};

class LambdaLVTConst
{

  /* Last Value Table (PC -> Last Value): Ideal model */
  std::map<uint64_t, uint64_t> lvt;
  public:
  LambdaLVTConst() {};

  bool check_and_insert(Cva6DynInstPtr inst){
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
};

void
PureBlock::commit(Cva6DynInstPtr inst) {
  /* Prediction stage */
  static int predictionttl = -1;
  static bool is_prediction = false;
  static lambdakto1_t prediction;

  uint64_t pc = inst->pc->instAddr();
  Cva6DynInstPtr i2 = nullptr;

  /* Fetch */
  if (predictionttl == -1){ /* Can perform prediction */
    if (llt.count(pc)){ /* Have prediction */
      prediction = llt[pc];
      predictionttl = prediction.size-1;
      is_prediction = true;
      // DPRINTF(Cva6LambdaLearn, "Predict %s\n", prediction.str());
    }
  }
  /* Annotate predicted instructions */
  if (is_prediction){
    inst->l_data.is_predicted_first = predictionttl == prediction.size-1;
    inst->l_data.is_predicted_last = predictionttl == 0;
    inst->l_data.is_predicted = true;
    inst->l_data.lambda = prediction;
  }
  if (inst->l_data.is_predicted_last){
    is_prediction = false;
  }

  /* tick */
  if (predictionttl != -1) {
    predictionttl -= 1;
  }
  if (inst->l_data.is_predicted_last){ /* Uop check inserttion */
    /* Create Static inst */
    StaticInstPtr si = new LambdaCheckInst(i2id(prediction.rd),
      prediction.rd_val);
    assert(si);
    /* Create the compound Dynamic instruction */
    i2 = new Cva6DynInst(&cpu, si, &inst->pc);
    i2->id.fetchSeqNum = 1; // Setup fake sequence number
    assert(i2->staticInst);
    // DPRINTF(Cva6LambdaLearn, "Issue %s\n", *i2);
  }

  /* Somewehere between fetch and commit */
  if (inst->l_data.is_predicted_last){ /* Check PC end */
    inst->l_data.do_check_pc_next(inst->pc_next->instAddr());
  }



  static uint64_t last_landable_cpt = 0;
  /* Primary check */
  bool isLambdable = isInstLamdable(inst);
  if (isLambdable){
    last_landable_cpt += 1;
  } else {
    last_landable_cpt = 0;
  }

  /* Validation stage (checker) Optional ?*/
  static BinaryLambdaRegFile rf;
  if (inst->l_data.is_predicted_first){
    rf.clear();
  }
  if (inst->l_data.is_predicted){
    rf.push(inst);
  }
  if (inst->l_data.is_predicted_last){/* If we reach end of lambda */
    bool is_check_regalloc = rf.isKto1();

    assert(i2);
    assert(i2->numSrcRegs() == 1);
    const RegId &reg = i2->srcRegIdx(0); /* Issue stage instruction */
    uint64_t value = cpu.thread->getReg(reg); /* Read Register */
    i2->setSrcRegOperand(0, inst->l_data.check_val);
    i2->executeComplete(); /* Execution stage */
    i2 = nullptr; /* Free instruction */

    bool valid = inst->l_data.do_final_check(value,
      last_landable_cpt, is_check_regalloc);
    statsp.req += 1;
    statsp.hit += valid;
    statsp.miss += !valid;
    statsp.miss_indempotance += !inst->l_data.is_check_indempotance;
    statsp.miss_val += !inst->l_data.is_check_val;
    statsp.miss_pc += !inst->l_data.is_check_pc;
    if (valid){
      statsp.hitLsize.sample(inst->l_data.lambda.size);
    }
  }

  /* Learning */
  stats.commit += 1;
  // inst_state_handler.commit(inst);
  /* Learn */
  /* Annotate if instruction is const */
  static LambdaLVTConst lvt;
  inst->l_data.is_const = lvt.check_and_insert(inst);

  if (!isInstLamdable(inst) || !inst->l_data.is_const) {
    pushLambda();
    inst_lambda_dump(inst);
  } else {
    window.push_back(inst);
  }
}

void
PureBlock::dump(){
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
  // for (auto& x: lambdaBtb){
  //   const lambdakto1_t &lambda = x.first;
  //   uint64_t nb_replay = x.second;
  //   int score = nb_replay * lambda.size;
  //   auto e = LS_t(score, lambda);
  //   if (q.size() < PQSIZE) {
  //       q.push(e);
  //   } else if (q.top().first < e.first){
  //       q.pop();
  //       q.push(e);
  //   }
  // }
  // for (; !q.empty(); q.pop()){
  //   auto e = q.top();
  //   std::cout << " * Best lambda is" <<  e.second.str();
  //   std::cout << " replay = " << e.first/e.second.size;
  //   std::cout << " score = " << e.first << std::endl;
  // }

  // /* Print best score */
  // lambdakto1_t best;
  // int best_score = -1;
  // for (auto& x: lambdaBtb){
  //   const lambdakto1_t &lambda = x.first;
  //   uint64_t nb_replay = x.second;
  //   int score = nb_replay * lambda.size;
  //   if (score > best_score){
  //     best = lambda;
  //     best_score = score;
  //   }
  // }
  // if (best_score){
  //   std::cout << "Best lambda is" <<  best.str();
  //   std::cout << " replay = " << best_score/best.size;
  //   std::cout << " score = " << best_score << std::endl;
  // }
  // for (auto const& x : lambdaBtb)
  // {
  //   const lambda_t &lambda = x.first;
  //   uint64_t nb_replay = x.second;

  //   total_inst += nb_replay * lambda.size;
  //   total_block += nb_replay;
  //   DPRINTF(Cva6LambdaDump, "[%6ld] : %s\n",
  //     nb_replay, lambda.str());
  // }
  // DPRINTF(Cva6LambdaDump, "#I = %ld, #LI = %ld, Block = %ld, MEAN = %f\n",
  //   stats.commit, total_inst,
  //   total_block, (float)total_inst/total_block);
}

}
}
