/**
 * reg_dead.hh
 *
 *  Author:   Pierre Ravenel
 * Created:   03/09/2024
 **/
#pragma once

#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/registerfile.hh"

namespace gem5 {
namespace cva6 {

class RegDeadAnayser : public Named
{
  protected:
  Cva6CPU &cpu;
  std::map<uint64_t /*pc*/, BinaryRegisterFile /*reg dead*/> rdmap;

  /* Statistics */
  BinaryRegisterFile rf_used;
  RegisterFile<Cva6DynInstPtr> rf_freeer;

  struct RegDeadAnayserStats : public statistics::Group
  {
    statistics::Scalar req;
    statistics::Scalar regdead;

    statistics::Scalar stores;
    statistics::Scalar stores_dead;

    RegDeadAnayserStats(Cva6CPU &cpu) :
      statistics::Group(&cpu, "rda"),
      ADD_STAT(req, "Request"),
      ADD_STAT(regdead, "RD Request"),
      ADD_STAT(stores, "Stores"),
      ADD_STAT(stores_dead, "Dead Stores")
    { }
  } stats;

  void init_rdmap(const char *elf);

  public:
  RegDeadAnayser(const std::string &name,
    Cva6CPU &cpu_,
    const BaseCva6CPUParams &params) :
    Named(name),
    cpu(cpu_),
    rf_freeer(cpu_),
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

  void clear(uint64_t pc, RegId reg){
    assert(rdmap.count(pc));
    // assert(rdmap[pc].isSet(reg));
    rdmap[pc].clear(reg);
  }

  bool check_reg_dead_at_commit(Cva6DynInstPtr inst);
};
} // namespace cva6
} // namespace gem5
