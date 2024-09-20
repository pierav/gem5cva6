/**
 * reg_dead.hh
 *
 *  Author:   Pierre Ravenel
 * Created:   03/09/2024
 **/
#pragma once

#include "cpu/cva6/registerfile.hh"

namespace gem5 {
namespace cva6 {

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


} // namespace cva6
} // namespace gem5