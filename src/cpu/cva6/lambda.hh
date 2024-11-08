/**
 * lambda.hh
 *
 *  Author:   Pierre Ravenel
 * Created:   19/09/2024
 **/

#pragma once

#include "base/named.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/lambda_types.hh"
#include "cpu/cva6/lambda_utils.hh"
#include "cpu/cva6/registerfile.hh"

namespace gem5 {
namespace cva6 {

class LambdaLVTConst
{
  /* Last Value Table (PC -> Last Value): Ideal model */
  std::map<uint64_t, uint64_t> lvt;
  public:
  LambdaLVTConst() {};
  bool check_and_insert(Cva6DynInstPtr inst);
};

class LambdaAlgo
{
  public:
  virtual lambdakto1_t predict(uint64_t pc) = 0;
  virtual void commit(Cva6DynInstPtr inst) = 0;
  virtual void evict(uint64_t pc) = 0;
};

class LambdaAlgoLLT : public LambdaAlgo
{
  protected:
  Cva6CPU &cpu;
  /* Heuristic to hint learner */
  LambdaLVTConst lvt;
  /* In flight instructions to learn */
  std::deque<Cva6DynInstPtr> window;
  /* Last Lambda Table */
  struct LLTEntry_t
  {
    lambdakto1_t lambda;
    struct sat_conf
    {
      int64_t conf = 0;
      void update_conf(bool valid){
        if (valid && conf < 4){
          conf += 1;
        }
        if (!valid && conf > -4){
          conf -= 1;
        }
      }
      bool valid(){
        return conf >= 0;
      }
      void invalidate(){
        conf = -4;
      }
      void reset(){
        conf = 0;
      }
    } conf;
    LLTEntry_t(lambdakto1_t &lambda_){
      lambda = lambda_;
    }
    LLTEntry_t() {}
  };
  std::map<uint64_t, LLTEntry_t> llt;
  /* Insert a lambda in LT */
  void pushLambda(uint64_t best_start, uint64_t best_end, uint64_t best_delta);
  /* Statistics */
  std::map<struct lambdakto1_t, size_t> lambdaBtb;
  /* Debug only to track sequence of instructions */
  int PQSIZE = 5; // number of indices we need
  std::map<lambdakto1_t, std::vector<Cva6DynInstPtr>*> saves;
  std::map<lambdakto1_t, int> savesscores;
  int minscore = 0;
  void dump();

  struct LambdaStats : public statistics::Group
  {
    /** Stats */
    statistics::Scalar commit;
    statistics::Scalar replayL;
    statistics::Distribution Lsize;
    LambdaStats(Cva6CPU &cpu) :
      statistics::Group(&cpu, "lambda"),
      ADD_STAT(commit, statistics::units::Count::get(), "commit"),
      ADD_STAT(replayL, statistics::units::Count::get(), "replayL"),
      ADD_STAT(Lsize, statistics::units::Count::get(), "Lsize")
    {
      Lsize
        .init(0,32,1)
        .flags(statistics::pdf);
    }
  } stats;

  /* Interface */
  public:
  LambdaAlgoLLT(Cva6CPU &cpu_) : cpu(cpu_), stats(cpu_) {}
  lambdakto1_t predict(uint64_t pc) override;
  void commit(Cva6DynInstPtr inst) override;
  void evict(uint64_t pc) override;

  /* Learnign functions */
  virtual void learnLambdaOnWindow();
};

class LambdaAlgoLLTLoadSlice : public LambdaAlgoLLT
{
  public:
  LambdaAlgoLLTLoadSlice(Cva6CPU &cpu_) : LambdaAlgoLLT(cpu_) {}
  void learnLambdaOnWindow() override;
};

class LambdaAlgoLLTKTo0 : public LambdaAlgoLLT
{
  public:
  LambdaAlgoLLTKTo0(Cva6CPU &cpu) : LambdaAlgoLLT(cpu) {}
  void commit(Cva6DynInstPtr inst) override;
  void learnLambdaOnWindow() override;
  virtual bool isTrigger(Cva6DynInstPtr inst){
    return inst->staticInst->isControl();
  }
};

class LambdaAlgoLLTSilentStorek0 : public LambdaAlgoLLTKTo0
{
  public:
  LambdaAlgoLLTSilentStorek0(Cva6CPU &cpu) : LambdaAlgoLLTKTo0(cpu) {}
  bool isTrigger(Cva6DynInstPtr inst) override {
    return inst->staticInst->isStore();
  }
};

class LambdaAlgoLLTInstAlone : public LambdaAlgoLLT
{
  public:
  LambdaAlgoLLTInstAlone(Cva6CPU &cpu_) : LambdaAlgoLLT(cpu_) {}
  void commit(Cva6DynInstPtr inst) override;
  virtual bool isTrigger(Cva6DynInstPtr inst) {
    return inst->staticInst->isControl();
  }
};

class LambdaAlgoLLTSilentStoreAlone : public LambdaAlgoLLTInstAlone
{
  public:
  LambdaAlgoLLTSilentStoreAlone(Cva6CPU &cpu_) :
    LambdaAlgoLLTInstAlone(cpu_) {}
  bool isTrigger(Cva6DynInstPtr inst) override {
    return inst->staticInst->isStore();
  }
};

class LambdaAlgoLLTLoadConstAlone : public LambdaAlgoLLTInstAlone
{
  public:
  LambdaAlgoLLTLoadConstAlone(Cva6CPU &cpu_) :
    LambdaAlgoLLTInstAlone(cpu_) {}
  bool isTrigger(Cva6DynInstPtr inst) override {
    inst->l_data.is_const = lvt.check_and_insert(inst);
    return inst->staticInst->isLoad() && inst->l_data.is_const;
  }
};

class LambdaHandler : public Named
{
  /* */
  Cva6CPU &cpu;
   /* */
  LambdaAlgo &algo;

  /* *** Fetch *** */
  private:
  int predictionttl = -1;
  bool in_lambda = false;

  lambdakto1_t prediction;
  /* *** Commit *** */
  uint64_t last_landable_cpt = 0;
  BinaryLambdaRegFile rf;

  struct LambdaPredStats : public statistics::Group
  {
    statistics::Scalar req;
    statistics::Scalar hit;
    statistics::Scalar miss;
    statistics::Scalar miss_indempotance;
    statistics::Scalar miss_val;
    statistics::Scalar miss_pc;
    statistics::Distribution hitLsize;
    LambdaPredStats(Cva6CPU &cpu) :
      statistics::Group(&cpu, "lambdapred"),
      ADD_STAT(req, statistics::units::Count::get(), "req"),
      ADD_STAT(hit, statistics::units::Count::get(), "hit"),
      ADD_STAT(miss, statistics::units::Count::get(), "miss"),
      ADD_STAT(miss_indempotance,
        statistics::units::Count::get(), "miss_indempotance"),
      ADD_STAT(miss_val, statistics::units::Count::get(), "miss_val"),
      ADD_STAT(miss_pc, statistics::units::Count::get(), "miss_pc"),
      ADD_STAT(hitLsize, statistics::units::Count::get(), "hitLsize")
    {
      hitLsize
        .init(0,32,1)
        .flags(statistics::pdf);
    }
  } stats;

  static LambdaAlgo &selectAlgo(Cva6CPU &cpu,
    const BaseCva6CPUParams &params){
      // return *new LambdaAlgoLLT(cpu);
      // return *new LambdaAlgoLLTLoadSlice(cpu);
      // return *new LambdaAlgoLLTbranchAlone(cpu);
      // return *new LambdaAlgoLLTKTo0(cpu);
      return *new LambdaAlgoLLTSilentStoreAlone(cpu);
      // return *new LambdaAlgoLLTSilentStorek0(cpu);
      // return *new LambdaAlgoLLTLoadConstAlone(cpu);
  }

  uint64_t lltSize;
  public:
  LambdaHandler(
      const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
    Named(name),
    cpu(cpu_),
    algo(selectAlgo(cpu_, params)),
    stats(cpu_), lltSize(params.lltSize) {}

  void on_fetch(Cva6DynInstPtr inst);

  void on_noisy_store(Cva6DynInstPtr inst);
  bool on_commit(Cva6DynInstPtr inst);
  void on_post_commit(Cva6DynInstPtr inst);

  Cva6DynInstPtr newPredInst(Cva6DynInstPtr);

  void flush_fetch(){
    predictionttl = -1;
    in_lambda = false;
  }
};

} // namespace cva6
} // namespace gem5
