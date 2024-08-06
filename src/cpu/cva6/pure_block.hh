#pragma once

#include <map>

#include "cpu/cva6/buffers.hh"
#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/misc/lru_containers.hh"
#include "cpu/cva6/registerfile.hh"

namespace gem5 {
namespace cva6 {

#define NB_INFLIGHT 32

//<PC, rs1, rs2, rs3, rd>
struct inststate_t
{
  uint64_t regs[5] = {0};
  inststate_t(Cva6DynInstPtr inst);
  inststate_t() {}
  bool operator==(const struct inststate_t& o) const {
    return memcmp(this, &o, sizeof(struct inststate_t)) == 0;
  }
  bool operator<(const struct inststate_t& o) const {
    return memcmp(this, &o, sizeof(struct inststate_t)) < 0;
  }
  size_t dohash() const {
    assert(sizeof(inststate_t) == 8*5);
    uint64_t res = 0;
    for (int i = 0; i < 5; i++){
      res ^= regs[i] << i;
    }
    return res;
  }
};


struct inststate_hash_t
{
  size_t operator()(const inststate_t& p) const {
    return p.dohash();
  }
};

struct fifo_t
{
  inststate_t buff[NB_INFLIGHT];
  // Only for backup singleton
  uint64_t dohash(uint64_t n) const {
    uint64_t res = 0;
    for (int i = 0; i < n; i++){
      res ^= buff[i].dohash();
    }
    return res;

  }
  bool operator==(const struct fifo_t& o) const {
    return memcmp(this, &o, sizeof(struct fifo_t)) == 0;
  }
  void push(inststate_t *state){
    /* insert state in the fifo */
    for (int i = NB_INFLIGHT - 1; i > 0; i--){
      buff[i] = buff[i-1];
    }
    buff[0] = *state;
  }
};

class fifo_hash_t
{
public:
    size_t operator()(const fifo_t& p) const {
      return p.dohash(NB_INFLIGHT);
    }
};

struct chunck_t
{
  const fifo_t *unique_fifo;
  size_t size;
  chunck_t() {}
  chunck_t(const fifo_t *fifo, size_t size_) :
    unique_fifo(fifo), size(size_) {}

  bool operator==(const struct chunck_t& o) const {
    assert(size == o.size);
    return memcmp(unique_fifo, o.unique_fifo, sizeof(inststate_t) * size) == 0;
  }

  uint64_t dohash() const {
    return unique_fifo->dohash(size);
  }
};


class inststaten_hash_t
{
public:
    size_t operator()(const chunck_t& p) const {
      return p.dohash();
    }
};


#define HISTSIZE 1000000

class InstStatesHandler
{
  /* The current fifo */
  fifo_t fifo;
  /* Backup used to perform singletons of fifo */
  std::unordered_set<fifo_t, fifo_hash_t> backup;
  // lru_set<fifo_t, fifo_hash_t> backupn;
  /**/
  chunck_t chuncks[NB_INFLIGHT];
  std::unordered_map<chunck_t, uint64_t, inststaten_hash_t>
    saves[NB_INFLIGHT];

  lru_suffix_tree<inststate_t, inststate_hash_t> suffixtree;

  struct InstStatesHandlerStats : public statistics::Group
  {
    statistics::Scalar req;
    statistics::Vector replay;
    InstStatesHandlerStats(Cva6CPU &cpu) :
      statistics::Group(&cpu, "states"),
      ADD_STAT(req, "Requests"),
      ADD_STAT(replay, "Replays")
    { replay.init(NB_INFLIGHT+1); }
  } stats;

  public:

  InstStatesHandler (Cva6CPU &cpu) :
    // backupn(HISTSIZE),
    suffixtree(NB_INFLIGHT, HISTSIZE),
    stats(cpu) {
  }

  void commit(inststate_t *state){
    size_t depth = suffixtree.insert(*state);
    assert(depth <= NB_INFLIGHT);
    for (int i = 0; i <= depth; i++){
      stats.replay[i] += 1;
    }
    stats.req += 1;
    return;
    #if 0
    /* insert state in the fifo */
    fifo.push(state);
    /* Check area */
    // uint64_t area_gb = sizeof(fifo_t) * backup.size();
    // if (area_gb > 10ULL << 30){
    //   fatal("Attention ca va peter : %d ", backup.size());
    // }
    /* Get singleton*/
    // auto tuitb = backup.insert(fifo);
    // const fifo_t *singleton_fifo = &(*tuitb.first);
    // // printf("Get backup new ? %d \n", tuitb.second);

    const fifo_t *singleton_fifo = backupn.insert(fifo);

    /* Compute stats */
    for (int i = 0; i < NB_INFLIGHT; i++){
      assert(saves[i].size() < HISTSIZE+10); // delayed assert
      chunck_t ch = chunck_t(singleton_fifo, i+1);
      // for (auto &e: saves[i]){
      //   printf("%lx, ", e.first.dohash());
      // }
      // printf("\n");
      if (saves[i].count(ch)){
        stats.replay[i] += 1;
        saves[i][ch] += 1;
      } else {
        saves[i][ch] = 1;
      }
      // printf("SAVES[%d]=", i);
    }

    fifo_t evicted_fifo;
    if (backupn.is_overflow(evicted_fifo)){
      // printf("Evict: H=%lx\n", evicted_fifo.dohash(NB_INFLIGHT));
      for (int i = 0; i < NB_INFLIGHT; i++){
        chunck_t ch = chunck_t(&evicted_fifo, i+1);
        // printf("Erase : (%ld) at i=%d size=%ld H=%lx\n",
        //   saves[i].count(ch), i, saves[i].size(), ch.dohash());
        if (saves[i].count(ch)){ // Erase if present
          saves[i].erase(ch);
        }
      }
      // Finnally free key
      backupn.evict();
    }
   #endif
  }
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

class RegDeadAnayser : public Named
{
  protected:
    Cva6CPU &cpu;
    std::map<uint64_t /*pc*/, BinaryRegisterFile /*reg dead*/> rdmap;
    struct RegDeadAnayserStats : public statistics::Group
    {
      statistics::Scalar req;
      statistics::Scalar regdead;
      RegDeadAnayserStats(Cva6CPU &cpu) :
        statistics::Group(&cpu, "rda"),
        ADD_STAT(req, statistics::units::Count::get(), "Request"),
        ADD_STAT(regdead, statistics::units::Count::get(), "RD Request")
      { }
    } stats;

    void init_rdmap(const char *elf);

  public:
    RegDeadAnayser(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Named(name),
      cpu(cpu_),
      stats(cpu_)
    {
      if (!params.userelf.empty()){
        init_rdmap(params.userelf.c_str());
      }
    }

    bool isRegDead(uint64_t pc, RegId reg){
      bool ret = rdmap.count(pc) && rdmap[pc].isSet(reg);
      stats.req += 1;
      stats.regdead += ret;
      return ret;
    }
};

class PureBlock : public Named
{
  public:

    Cva6CPU &cpu;
    RegDeadAnayser rda;

    /* In flight learn */
    BinaryRegisterFile rfsrc;
    BinaryRegisterFile rfdst;
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

    std::map<inststate_t, uint64_t> infiniteBtb;
    InstStatesHandler inst_state_handler;
    uint64_t pctrigger;
    uint64_t pccnt;

    RegisterFile<bool> register_src;
    RegisterFile<bool> register_dst;

    RegisterFile<uint64_t> register_src_val;
    RegisterFile<uint64_t> register_dst_val;

    std::map<struct lambda_t, uint64_t> lambdaBtb;

    struct LambdaStats : public statistics::Group
    {
      /** Stats */
      statistics::Scalar commit;
      statistics::Scalar replayI;
      statistics::Scalar replayI2;
      statistics::Scalar replayI3;
      statistics::Scalar replayL;

      statistics::Distribution Lsize;
      LambdaStats(Cva6CPU &cpu) :
        statistics::Group(&cpu, "lambda"),
        ADD_STAT(commit, statistics::units::Count::get(), "commit"),
        ADD_STAT(replayI, statistics::units::Count::get(), "replayI"),
        ADD_STAT(replayI2, statistics::units::Count::get(), "replayI2"),
        ADD_STAT(replayI3, statistics::units::Count::get(), "replayI3"),
        ADD_STAT(replayL, statistics::units::Count::get(), "replayL"),
        ADD_STAT(Lsize, statistics::units::Count::get(), "Lsize")
      {
        Lsize
          .init(0,16,1)
          .flags(statistics::pdf);
      }
    } stats;

   protected:
    inststate_t inststateInit(Cva6DynInstPtr inst);
    void pushLambda();

   public:
    PureBlock(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Named(name),
      cpu(cpu_),
      rda(name, cpu_, params),
      state(Idle),
      inst_state_handler(cpu_),
      register_src(cpu),
      register_dst(cpu),
      register_src_val(cpu),
      register_dst_val(cpu),
      stats(cpu)
      { }

    bool lookup(Cva6DynInstPtr inst){
      return true;
    }

    void commit(Cva6DynInstPtr inst);
    void dump();
};

} // namespace cva6
} // namespace gem5
