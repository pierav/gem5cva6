#pragma once

#include <map>

#include "cpu/cva6/buffers.hh"
#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/misc/lru_containers.hh"
#include "cpu/cva6/misc/memory.hh"
#include "cpu/cva6/registerfile.hh"

namespace gem5 {
namespace cva6 {

#define NB_INFLIGHT 32
#define HISTSIZE 1000000

using state_t = Cva6DynInst::inststate_t;

class InstStatesHandler
{
  lru_suffix_tree<const state_t*> suffixtree;
  std::set<state_t> memsave;
  std::deque<Cva6DynInstPtr> committed;

  struct InstStatesHandlerStats : public statistics::Group
  {
    statistics::Scalar req;
    statistics::Scalar replayI;
    statistics::Vector replay;
    statistics::Vector replay_cl;
    statistics::Vector replay_ss;
    statistics::Vector replay_clss;

    statistics::Vector replay_rd;
    statistics::Vector replay_rdk1;

    InstStatesHandlerStats(Cva6CPU &cpu) :
      statistics::Group(&cpu, "states"),
      ADD_STAT(req, "Requests"),
      ADD_STAT(replay, "Replays"),
      ADD_STAT(replay_cl, "replay_cl"),
      ADD_STAT(replay_ss, "replay_ss"),
      ADD_STAT(replay_clss, "replay_clss"),
      ADD_STAT(replay_rd, "replay_clss"),
      ADD_STAT(replay_rdk1, "")
    {
      replay.init(NB_INFLIGHT+1);
      replay_cl.init(NB_INFLIGHT+1);
      replay_ss.init(NB_INFLIGHT+1);
      replay_clss.init(NB_INFLIGHT+1);
      replay_rd.init(NB_INFLIGHT+1);
      replay_rdk1.init(NB_INFLIGHT+1);
    }
  } stats;

  public:
  InstStatesHandler (Cva6CPU &cpu) :
    // backupn(HISTSIZE),
    suffixtree(NB_INFLIGHT, HISTSIZE),
    committed(NB_INFLIGHT),
    stats(cpu) {
  }
  void commit(Cva6DynInstPtr inst);
};

class LambdaPredictor: public Named
{
  struct LambdaPredEntry
  {
    uint64_t tag = 0;
    uint64_t cpt = 0;

    uint64_t rs1 = 0;
    uint64_t rs2 = 0;
    uint64_t rs3 = 0;

    uint64_t rd = 0;
  };

  LambdaPredEntry entries[1024];

  public:
    Cva6CPU &cpu;
    LambdaPredictor(const std::string &name, Cva6CPU &cpu_) :
      Named(name),
      cpu(cpu_) {}

    bool predict(uint64_t pc){
      LambdaPredEntry &e = entries[pc % 1024];
      return e.tag == pc && e.cpt > 3;
    }

    void commit(uint64_t pc, bool outcome){
      LambdaPredEntry &e = entries[pc % 1024];
      if (outcome && e.cpt < 3){
        e.cpt += 1;
      } else if (!outcome && e.cpt > 0){
        e.cpt -= 1;
      }
    }

};

// class LambdaValueTable: public Named{
// };


class PureBlock : public Named
{
  public:

    Cva6CPU &cpu;

    /* In flight learn */
    std::deque<Cva6DynInstPtr> window;

    enum PureBlockState
    {
      Idle = 0,
      Append,
      WaitEnd
    };

    struct regval_t
    {
      RegId reg;
      uint64_t value;
    };


    struct lambda_t
    {
      uint64_t pc = 0;
      uint64_t size = 0;
      uint64_t id_src_mask = 0;
      uint64_t src[4] = { 0 };
      uint64_t id_dst_mask = 0;
      uint64_t dst[4] = { 0 };

      bool operator==(const struct lambda_t& o) const {
        return memcmp(this, &o, sizeof(struct lambda_t)) == 0;
      }

      bool operator<(const struct lambda_t& o) const {
        return memcmp(this, &o, sizeof(struct lambda_t)) < 0;
      }

      std::string str() const {
        std::ostringstream os;

        // Lambda address
        os <<  "\033[32m" << "@" << this->pc
          << '<' << this->size << '>';
        os << std::hex;
        os << '(' << "\033[39m";
        os << BinaryRegisterFile(id_src_mask).dump(src);

        os << "\033[32m" << ')' << "\033[39m";
        os << "\033[32m" << " |-> " << "\033[39m";

        // Lambda output
        os << BinaryRegisterFile(id_dst_mask).dump(dst);

        return os.str();
      }

    };

    enum PureBlockState state;

    InstStatesHandler inst_state_handler;
    uint64_t pctrigger;
    uint64_t pccnt;



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
    } statsp;
   protected:
    void pushLambda();

   public:
    PureBlock(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Named(name),
      cpu(cpu_),
      state(Idle),
      inst_state_handler(cpu_),
      stats(cpu),
      statsp(cpu)
      { }

    bool lookup(Cva6DynInstPtr inst){
      return true;
    }

    void commit(Cva6DynInstPtr inst);
    void dump();
};

} // namespace cva6
} // namespace gem5
