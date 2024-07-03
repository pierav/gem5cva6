#pragma once

#include <map>

#include "cpu/cva6/buffers.hh"
#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/registerfile.hh"

namespace gem5 {
namespace cva6 {

//<PC, rs1, rs2, rs3, rd>
struct inststate_t
{
  uint64_t regs[5] = {0};
  inststate_t(Cva6DynInstPtr inst);
  bool operator==(const struct inststate_t& o) const {
    return memcmp(this, &o, sizeof(struct inststate_t)) == 0;
  }
  bool operator<(const struct inststate_t& o) const {
    return memcmp(this, &o, sizeof(struct inststate_t)) < 0;
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

    void init_rdmap(const char *elf);

  public:
    RegDeadAnayser(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Named(name),
      cpu(cpu_) {
      if (!params.userelf.empty()){
        init_rdmap(params.userelf.c_str());
      }
    }

    bool isRegDead(uint64_t pc, RegId reg){
      return rdmap.count(pc) && rdmap[pc].isSet(reg);
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
      statistics::Scalar replayL;

      statistics::Distribution Lsize;
      LambdaStats(Cva6CPU &cpu) :
        statistics::Group(&cpu, "lambda"),
        ADD_STAT(commit, statistics::units::Count::get(), "commit"),
        ADD_STAT(replayI, statistics::units::Count::get(), "replayI"),
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
