/**
 * @file fu_base.hh
 * @author Pierre Ravenel (pravenel@kalrayinc.com)
 * @brief Base interface for functional unit
 * @version 1.0
 * @date 2023-05-25
 */

#include <ostream>
#include <sstream>
#include <string>

#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/pipe_data.hh"
#include "cpu/func_unit.hh"
#include "debug/Cva6FU.hh"

#pragma once

namespace gem5 {
namespace cva6 {

/** Ready/Valid interface used by fus */
class ReadyValidIntf
{
  public:
    /** Input interface */
    virtual bool canPush(Cva6DynInstPtr inst) = 0;
    virtual void push(Cva6DynInstPtr inst) = 0;
    /** Advance the FU (1 cycle) */
    virtual bool advance(void) = 0;
    /** Output interface */
    virtual bool canPop(Cva6DynInstPtr inst) = 0;
    virtual void pop(Cva6DynInstPtr inst) = 0;
    /** Flush */
    virtual void flushfrom(Cva6DynInstPtr inst) = 0;
};

/** Ready/Valid interface aggregation */
class ReadyValidIntfSplit : public ReadyValidIntf
{
  protected:
  std::vector<ReadyValidIntf*> rvs;
  virtual ReadyValidIntf *du(Cva6DynInstPtr inst) = 0;
  public:
  bool canPush(Cva6DynInstPtr inst){ return du(inst)->canPush(inst); }
  void push(Cva6DynInstPtr inst){ return du(inst)->push(inst); }
  bool canPop(Cva6DynInstPtr inst){ return du(inst)->canPop(inst); }
  void pop(Cva6DynInstPtr inst){ return du(inst)->pop(inst); }
  void flushfrom(Cva6DynInstPtr inst){
    for (auto rv: rvs){ rv->flushfrom(inst); }
  }
  bool advance(){
    bool ret = false;
    for (auto rv: rvs){ ret |= rv->advance(); }
    return ret;
  }
};

/** FU base with a R/V intf */
class FUBase : public virtual ReadyValidIntf, /* Ready valid interface */
               public FuncUnit,       /* Base func unit */
               public Named {
  public:
    FUBase(const std::string &name_, std::vector<OpClass> &ops) :
      Named(name_)
    {
      // NO MORE TODO
      /* All pipelines should be able to execute No_OpClass instructions */
      // addCapability(No_OpClass, 1, 1);
      /* Add the capabilities */
      for (unsigned int i = 0; i < ops.size(); i++) {
          addCapability(ops[i], 1, 1);
      }
    }

    std::string dump(void){
        std::ostringstream data;
        data << "FU(" << name() << ")";
        return data.str();
    }

    /** Default is Named interface */
    std::string name() const {
      return Named::name();
    }
};


} // namespace cva6
} // namespace gem5
