
/**
 * @file
 *
 * Base interface for functional unit
 *
 */

#include <ostream>
#include <sstream>
#include <string>

#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/func_unit.hh"
#include "debug/Cva6FU.hh"

#pragma once

namespace gem5 {
namespace cva6 {

/** Interface Ready/Valid used by fus */
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

class Cva6DynInstChunk : public Named
{
  using ContainerT = std::deque<Cva6DynInstPtr>;

  protected:
    ContainerT chunk;


  public:
    Cva6DynInstChunk(const std::string &name) : Named(name){ }

    bool canPush(Cva6DynInstPtr inst){
      // return chunk.size() != 1;
      return true;
    }

    void push(Cva6DynInstPtr inst){
      chunk.push_back(inst);
    }

    bool canPop(Cva6DynInstPtr inst){
      return std::find(chunk.begin(), chunk.end(), inst) != chunk.end();
    }

    void pop(Cva6DynInstPtr inst){
      chunk.erase(std::find(chunk.begin(), chunk.end(), inst));
    }

    void flushfrom(Cva6DynInstPtr _inst) {
      while (!chunk.empty() &&
        chunk.back()->isAfterOrEqual(_inst)){
        DPRINTF(Cva6FU, "Flush %s\n", *chunk.back());
        chunk.pop_back();
      }
    }
    bool empty()                 { return chunk.empty(); }
    size_t size()                { return chunk.size(); }
    Cva6DynInstPtr front()       { return chunk.front(); }
    Cva6DynInstPtr back()        { return chunk.back(); }
    ContainerT::iterator begin() { return chunk.begin(); }
    ContainerT::iterator end()   { return chunk.end(); }

    Cva6DynInstPtr& operator[](int idx)      { return chunk[idx]; }
    Cva6DynInstPtr operator[](int idx) const { return chunk[idx]; }


};

} // namespace cva6
} // namespace gem5
