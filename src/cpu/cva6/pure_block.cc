#include "cpu/cva6/pure_block.hh"

#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>

#include "arch/riscv/utility.hh"
#include "cpu/cva6/registerfile.hh"
#include "debug/Cva6Commit.hh"
#include "debug/Cva6LambdaCommit.hh"
#include "debug/Cva6LambdaDump.hh"
#include "debug/Cva6LambdaLearn.hh"
#include "debug/Cva6LambdaRDA.hh"

namespace gem5 {
namespace cva6 {


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


struct lambdakto1_t
{
  uint64_t pc_start;
  uint64_t pc_end;
  uint64_t size;
  uint64_t rd;
  uint64_t rd_val;

  bool operator==(const struct lambdakto1_t& o) const {
    return memcmp(this, &o, sizeof(lambdakto1_t)) == 0;
  }

  bool operator<(const struct lambdakto1_t& o) const {
    return memcmp(this, &o, sizeof(lambdakto1_t)) < 0;
  }

  std::string str() const {
    std::ostringstream os;

    // Lambda address
    os << std::hex;
    os <<  "\033[32m" << "@" << pc_start << "->" <<  pc_end;
    os << '<' << size << '>';
    // Lambda input
    os << '(' << "\033[39m";
    os << '*';
    os << "\033[32m" << ')' << "\033[39m";
    os << "\033[32m" << " |-> " << "\033[39m";
    // Lambda output
    os << '{' << registerName(rd) << ": " << rd_val << '}';
    return os.str();
  }
};


class BinaryLambdaRegFile
{
  protected:
  BinaryRegisterFile rfsrc;
  BinaryRegisterFile rfdst;

  public:
  BinaryLambdaRegFile() {};

  virtual void rfsrc_set_val(RegId reg, uint64_t value) {}
  virtual void rfdst_set_val(RegId reg, uint64_t value) {}

  void push(Cva6DynInstPtr inst) {
    assert(!inst->isFault());
    // Append all src regs
    for (unsigned int i = 0; i < inst->staticInst->numSrcRegs(); i++) {
      RegId reg = inst->staticInst->srcRegIdx(i);
      if (reg.classValue() != InvalidRegClass){
        if (!rfdst.isSet(reg)){ /* Is register lambda input */
          rfsrc.set(reg);
          rfsrc_set_val(reg, inst->getSrcRegOperand(i));
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
        rfdst_set_val(reg, inst->getDstRegOperand(i));
      }
    }
  }

  void clear(){
    rfsrc.clear();
    rfdst.clear();
  }

  void isXtoY(int x, int y){
    fatal("Unimplemented!");
  }

  bool isKto1(){
    return rfdst.popcount() <= 1;
  }

  bool isRdSingle(){
    return rfdst.isSingle();
  }

  std::string dump(){
    std::stringstream ss;
    ss << "L:" << rfsrc.dump() << "->" << rfdst.dump();
    return ss.str();
  }
};

class LambdaRegFile : public BinaryLambdaRegFile
{
  RegisterFile<uint64_t> register_src_val;
  RegisterFile<uint64_t> register_dst_val;
  public:
  LambdaRegFile(BaseCPU &cpu) :
    BinaryLambdaRegFile(),
    register_src_val(cpu),
    register_dst_val(cpu) { }

  void rfsrc_set_val(RegId reg, uint64_t value) override {
    register_src_val.set(reg, value);
  }
  void rfdst_set_val(RegId reg, uint64_t value) override {
    register_dst_val.set(reg, value);
  }
  std::pair<RegId, uint64_t> getSingle(){
    assert(isKto1());
    RegId reg = rfdst.getSingle();
    uint64_t val = register_dst_val.get(reg);
    return {reg, val};
  }
};

std::string srcdst(Cva6DynInstPtr inst){
  std::stringstream ss;
  for (unsigned int i = 0; i < inst->staticInst->numSrcRegs(); i++) {
    RegId reg = inst->staticInst->srcRegIdx(i);
    ss << riscvRegisterName(reg) << ':';
    if (inst->exec_data.is_reg_dead[i]){
      ss << "X";
    } else {
      ss << '.';
    }
    ss << ' ';
  }
  return ss.str();
}

static std::map<struct lambdakto1_t, size_t> lambdaBtb;

int PQSIZE = 5; // number of indices we need
using LS_t = std::pair<int, lambdakto1_t>; // score lambda
std::priority_queue<LS_t> q;

std::map<lambdakto1_t, std::vector<Cva6DynInstPtr>*> saves;
std::map<lambdakto1_t, int> savesscores;
int minscore = 0;

void
PureBlock::pushLambda(){

  if (window.size() < 1){
    return;
  }
  /* Compute best score in window */
  int best_start = 0;
  int best_delta = 0;
  for (int start = 0; start < window.size(); start++){
    /* Compute best end for iteration start */
    int iter_best_end = start;
    BinaryLambdaRegFile rf;
    for (int end = start; end < window.size(); end++){
      rf.push(window[end]);
      if (rf.isKto1()){
        iter_best_end = end;
      }
    }
    int iter_best_delta = iter_best_end - start + 1;
    /* Is better */
    if (iter_best_delta > best_delta) {
      best_start = start;
      best_delta = iter_best_delta;
    }
  }
  int best_end = best_start+best_delta-1;
  // DPRINTF(Cva6LambdaLearn,
  //   "*Best Score [[%d [%d:%d] %d[[ : %d/%d\n",
  //   0, best_start, best_end, window.size(),
  //   best_delta, window.size());
  assert(best_end < window.size());

  /* Compute Window */
  static LambdaRegFile rf(cpu);   /* Compute real register file */
  rf.clear();
  // DPRINTF(Cva6LambdaLearn, "*** learn on:\n");
  for (int i = 0; i < window.size(); i++){
    bool inWindow = i >= best_start && i <= best_end;
    if (inWindow){
       rf.push(window[i]);
    }
    DPRINTF(Cva6LambdaLearn,
      "*** WIN[%d] = %s %s %s\n",
      i, *window[i],
      inWindow ? rf.dump() : "",
      srcdst(window[i]));
  }

  fatal_if(!rf.isKto1(), "Must be k->1\n");

  /* Create Lambda */
  std::pair<RegId, uint64_t> pair =
    rf.isRdSingle() ? rf.getSingle()
    : std::pair<RegId, uint64_t>(RiscvISA::intRegClass[0], 0xcafe);
  lambdakto1_t lambda = {
    .pc_start = window[best_start]->pc->instAddr(),
    .pc_end = window[best_end]->pc->instAddr(),
    .size = best_delta,
    .rd = pair.first,
    .rd_val = pair.second
  };


  // Append Lambda
  if (lambdaBtb.count(lambda) > 0){
    stats.replayL += lambda.size;
    stats.Lsize.sample(lambda.size);
  }
  lambdaBtb[lambda] += 1; // Increment

  DPRINTF(Cva6LambdaLearn, "push Lambda [%d]: %s\n",
            lambdaBtb[lambda], lambda.str());

  int score =  lambdaBtb[lambda] * lambda.size;

  if (saves.count(lambda)){ /* Simply update */
    /* Check instruction flows */
    std::vector<Cva6DynInstPtr> &vec = *saves[lambda];
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


  window.clear();
}

bool isInstLamdable(Cva6DynInstPtr inst){
  return !inst->isFault() &&
         inst->exec_data.is_const_load &&
         inst->exec_data.is_silent_store;
}

void
PureBlock::commit(Cva6DynInstPtr inst) {

  /* Perform prediction */


  /* Learning */
  if (inst->isFault()){
    pushLambda();
    return;
  }

  stats.commit += 1;
  // inst_state_handler.commit(inst);
  /* Learn */
  if (!isInstLamdable(inst)) {
    pushLambda();
    DPRINTF(Cva6LambdaLearn, "*** ---[-] = %s\n", *inst);
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
