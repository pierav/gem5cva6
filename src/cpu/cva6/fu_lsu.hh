/**
 * @file fu_lsu.hh
 * @author Pierre Ravenel (pravenel@kalray.eu)
 * @brief
 * @version 0.1
 * @date 20/01/2023
 *
 */
#pragma once

#include <string>
#include <vector>

#include "base/named.hh"
#include "base/statistics.hh"
#include "cpu/cva6/buffers.hh"
#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/fu_base.hh"
#include "mem/packet.hh"

namespace gem5 {
namespace cva6 {


class MatchAddrIntf
{
  public:
  virtual bool isMaskMatchVaddr(Cva6DynInstPtr inst, uint64_t mask) = 0;
  bool isPageOffsetMatches(Cva6DynInstPtr inst){
      return isMaskMatchVaddr(inst, 0b111111111000);
  }
  bool isClMatch(Cva6DynInstPtr inst, uint64_t clsize){
      uint64_t mask = ((1 << 12) - 1); // 0b111111111111;
      mask &= ~(clsize - 1); // 0b111111110000
      // assert(mask == 0b111111110000);
      return isMaskMatchVaddr(inst, mask);
  }
};

// Store queue persists store requests and pushes them to memory
// if they are no longer speculative
class LSUStoreBuffer
    : public Named, public ReadyValidIntf, public MatchAddrIntf
{
  protected:
      Cva6CPU &cpu;
  public:

    // the store queue has two parts:
    // 1. Speculative queue
    // 2. Commit queue which is non-speculative
    //  : the store will definitely happen.
    const unsigned int depth_spec = 16;
    const unsigned int depth_commit = 16; // WT: 4; WB: 8
    std::deque<Cva6DynInstPtr> speculative_queue;
    Cva6DynInstChunk commit_queue;

  public:
    LSUStoreBuffer(const std::string &name,
                   Cva6CPU &cpu_) :
        Named(name),
        cpu(cpu_),
        commit_queue(name + ".SQc") { ; }

    // there is no store pending in neither the speculative unit or
    // the non-speculative queue
    bool isEmpty();

    // The load should return the data stored by the most recent store to the
    // same physical address.  The most direct way to implement this is to
    // maintain physical addresses in the store buffer.

    // Of course, there are other micro-architectural techniques to accomplish
    // the same thing: you can interlock and wait for the store buffer to
    // drain if the load VA matches any store VA modulo the page size (i.e.
    // bits 11:0).  As a special case, it is correct to bypass if the full VA
    // matches, and no younger stores' VAs match in bits 11:0.
    //
    // checks if the requested load is in the store buffer
    // page offsets are virtually and physically the same
    bool isMaskMatchVaddr(Cva6DynInstPtr inst, uint64_t mask) override;

    /* Ready Valid interface */
    bool canPush(Cva6DynInstPtr inst_);
    void push(Cva6DynInstPtr inst_);
    void flushfrom(Cva6DynInstPtr inst_);
    bool canPop(Cva6DynInstPtr inst_);
    void pop(Cva6DynInstPtr inst_);
    bool advance();
};

class LSUAmoBuffer
    : public Named, public ReadyValidIntf, public MatchAddrIntf
{
  public:
    Cva6CPU &cpu;
    Cva6DynInstPtr amo_buffer; // Fifo with 1 element :)
    bool amo_committed;
    LSUStoreBuffer *store_buffer; /** Pointer back to store buffer */

    LSUAmoBuffer(const std::string &name,
                 Cva6CPU &cpu_,
                 LSUStoreBuffer *store_buffer_) :
        Named(name),
        cpu(cpu_),
        amo_buffer(Cva6DynInst::bubble()),
        amo_committed(false),
        store_buffer(store_buffer_){ }

    bool canPush(Cva6DynInstPtr inst_);
    void push(Cva6DynInstPtr inst_);
    bool canPop(Cva6DynInstPtr inst_);
    void pop(Cva6DynInstPtr inst_);
    void flushfrom(Cva6DynInstPtr inst_);
    bool advance();

    bool isMaskMatchVaddr(Cva6DynInstPtr inst, uint64_t mask) override;
};

class LSUStoreUnit
    : public Named, public ReadyValidIntf, public MatchAddrIntf
{
  protected:
    Cva6CPU &cpu;
  public:
    LSUStoreBuffer store_buffer;
    LSUAmoBuffer amo_buffer;

  public:
    LSUStoreUnit(const std::string &name, Cva6CPU &cpu_)
        : Named(name),
          cpu(cpu_),
          store_buffer(name + ".store_buffer", cpu),
          amo_buffer(name + ".amo_buffer", cpu, &store_buffer) { ; }

  protected:
    ReadyValidIntf *destUnit(Cva6DynInstPtr inst){

        /*
        bool is_load = request->isLoad;
        bool is_llsc = request->request->isLLSC();
        bool is_release = request->request->isRelease();
        bool is_swap = request->request->isSwap();
        bool is_atomic = request->request->isAtomic();
        bool bufferable = !(request->request->isStrictlyOrdered() ||
                    is_llsc || is_swap || is_atomic || is_release);
        */

        if (inst->staticInst->isAtomic() ||
            inst->staticInst->isStoreConditional()){
            return &amo_buffer;
        } else {
            return &store_buffer;
        }
    }

  public:
    bool canPush(Cva6DynInstPtr inst){
        // We cannot push in store_buffer until
        // the amo_buffer is not empty
        return amo_buffer.canPush(inst) &&
               store_buffer.canPush(inst);
        // return destUnit(inst)->canPush(inst);
    }

    void push(Cva6DynInstPtr inst){
        assert(canPush(inst));
        destUnit(inst)->push(inst);
    }

    bool canPop(Cva6DynInstPtr inst){
        return destUnit(inst)->canPop(inst);
    }

    void pop(Cva6DynInstPtr inst){
        assert(canPop(inst));
        destUnit(inst)->pop(inst);
    }

    void flushfrom(Cva6DynInstPtr inst_){
        store_buffer.flushfrom(inst_);
        amo_buffer.flushfrom(inst_);
    }

    bool advance(){
        return store_buffer.advance() |
               amo_buffer.advance();
    }

    bool isMaskMatchVaddr(Cva6DynInstPtr inst, uint64_t mask) override {
      return store_buffer.isMaskMatchVaddr(inst, mask) ||
             amo_buffer.isMaskMatchVaddr(inst, mask);
    }
};

class LSULoadUnit
    : public Named, public ReadyValidIntf
{
  protected:
    Cva6DynInstChunk loadqueue;       /** Instructions in Load unit */
    Cva6DynInstChunk insts_in_memory; /** instructions in memory */
    LSUStoreUnit *su;                 /** Pointer back to store unit */
    Cva6CPU &cpu;                     /** Pointer back to CPU */
    struct LSULoadUnitStats : public statistics::Group
    {
      statistics::Scalar req;
      statistics::Scalar oooreq;
      LSULoadUnitStats(BaseCPU &cpu):
        statistics::Group(&cpu, "lsu"),
        ADD_STAT(req, statistics::units::Count::get(),
                 "Number of load issued"),
        ADD_STAT(oooreq, statistics::units::Count::get(),
                 "Number of load OoO issued"){}
    } stats;

  public:
    LSULoadUnit(const std::string &name,
                LSUStoreUnit *su_,
                Cva6CPU &cpu_) :
        Named(name),
        loadqueue(name + ".LQ"),
        insts_in_memory(name + ".LQF"),
        su(su_),
        cpu(cpu_),
        stats(cpu) { }

    bool canPush(Cva6DynInstPtr inst_);
    void push(Cva6DynInstPtr inst_);
    bool canPop(Cva6DynInstPtr inst_);
    void pop(Cva6DynInstPtr inst_);
    void flushfrom(Cva6DynInstPtr inst_);
    bool advance();
};

class LSUBase
    : public Named, public virtual ReadyValidIntf
{
  protected:
    Cva6CPU &cpu;
    LSUStoreUnit store_unit;
    LSULoadUnit load_unit;
    Cva6DynInstChunk lsu_fifo; /** Lsu bypass buffer */

  public:
    LSUBase(const std::string &name,
            Cva6CPU &cpu_,
            const BaseCva6CPUParams &params) :
        Named(name),
        cpu(cpu_),
        store_unit(name + ".store_unit", cpu),
        load_unit(name + ".load_unit", &store_unit, cpu),
        lsu_fifo(name + ".LSQ") { }

  protected:
    ReadyValidIntf *destUnit(Cva6DynInstPtr inst){
        assert(inst->staticInst->isMemRef());
        if (inst->staticInst->isLoad()){
            return &load_unit;
        } else {
            return &store_unit;
        }
    }

  public:
    bool canPush(Cva6DynInstPtr inst);
    void push(Cva6DynInstPtr inst);
    bool canPop(Cva6DynInstPtr inst);
    void pop(Cva6DynInstPtr inst);
    void flushfrom(Cva6DynInstPtr inst_);
    bool advance();
};


class LSUBaseChecker
    : public Named, public virtual ReadyValidIntf
{
  protected:
    Cva6CPU &cpu;
    Cva6DynInstChunk cqueue;  /** CheckQueue */
    Cva6DynInstChunk lsu_fifo; /** Lsu bypass buffer */
  public:
    LSUBaseChecker(const std::string &name,
            Cva6CPU &cpu_,
            const BaseCva6CPUParams &params) :
        Named(name),
        cpu(cpu_),
        cqueue(name + ".CQ"),
        lsu_fifo(name + ".LSQ")
        { }
    bool canPush(Cva6DynInstPtr inst);
    void push(Cva6DynInstPtr inst);
    bool canPop(Cva6DynInstPtr inst);
    void pop(Cva6DynInstPtr inst);
    void flushfrom(Cva6DynInstPtr inst_);
    bool advance();
};

class LSUWithCheckerLQ : public virtual ReadyValidIntf
{
  protected:
  LSUBase base;
  LSUBaseChecker checker;
  LSUWithCheckerLQ(const std::string &name,
            Cva6CPU &cpu_,
            const BaseCva6CPUParams &params):
            base(name, cpu_, params),
            checker(name, cpu_, params) {}
  ReadyValidIntf *du(Cva6DynInstPtr inst){
    if (inst->l_data.is_predicted){
      return &checker;
    }
    return &base;
  }
  bool canPush(Cva6DynInstPtr inst){ return du(inst)->canPush(inst); }
  void push(Cva6DynInstPtr inst){ return du(inst)->push(inst); }
  bool canPop(Cva6DynInstPtr inst){ return du(inst)->canPop(inst); }
  void pop(Cva6DynInstPtr inst){ return du(inst)->pop(inst); }
  void flushfrom(Cva6DynInstPtr inst){
    base.flushfrom(inst);
    checker.flushfrom(inst);
  }
  bool advance(){
    base.advance();
    checker.advance();
    return true;
  }
};

class FULSU
  : public LSUWithCheckerLQ, public FUBase
{
  public:
    FULSU(const std::string &name,
          std::vector<OpClass> &ops,
          Cva6CPU &cpu_,
          const BaseCva6CPUParams &params)
      : LSUWithCheckerLQ(name, cpu_, params),
        FUBase(name, ops)
        { ; }
};

} // namespace cva6
} // namespace gem5
