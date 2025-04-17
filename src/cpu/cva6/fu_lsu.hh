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
    unsigned int depth_spec = 16;
    unsigned int depth_commit = 16; // WT: 4; WB: 8
    std::deque<Cva6DynInstPtr> speculative_queue;
    Cva6DynInstChunk commit_queue;

  public:
    LSUStoreBuffer(const std::string &name,
                   Cva6CPU &cpu_,
                   const BaseCva6CPUParams &p) :
        Named(name),
        cpu(cpu_),
        depth_spec(p.lsuSQSWidth),
        depth_commit(p.lsuSQCWidth),
        commit_queue(name + ".SQc") { ; }

    // there is no store pending in neither the speculative unit or
    // the non-speculative queue
    bool isEmpty();

    // Enhanced version of isEmpty. Allow store to be issued
    bool isNoPendingReqBefore(Cva6DynInstPtr& inst);

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
    uint64_t lookupSQDW(Cva6DynInstPtr inst, uint64_t& value);

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
    : public Named, public ReadyValidIntfSplit, public MatchAddrIntf
{
  protected:
  Cva6CPU &cpu;
  public:
  LSUStoreBuffer store_buffer;
  LSUAmoBuffer amo_buffer;

  public:
  LSUStoreUnit(const std::string &name,
               Cva6CPU &cpu_,
               const BaseCva6CPUParams &p)
      : Named(name),
        cpu(cpu_),
        store_buffer(name + ".store_buffer", cpu, p),
        amo_buffer(name + ".amo_buffer", cpu, &store_buffer) {
    rvs = { &store_buffer, &amo_buffer };
  }

  protected:
  ReadyValidIntf *du(Cva6DynInstPtr inst) override {
    if (inst->staticInst->isAtomic() ||
        inst->staticInst->isStoreConditional()){
        return &amo_buffer;
    } else {
        return &store_buffer;
    }
  }

  public:
  bool isMaskMatchVaddr(Cva6DynInstPtr inst, uint64_t mask) override {
    return store_buffer.isMaskMatchVaddr(inst, mask) ||
            amo_buffer.isMaskMatchVaddr(inst, mask);
  }

  bool lookupSQ(Cva6DynInstPtr inst);
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


class LSULoadUnitNoLock
    : public Named, public ReadyValidIntf
{
  protected:
    LSUStoreUnit *su; /** Pointer back to store unit */
    Cva6CPU &cpu;     /** Pointer back to CPU */
    struct LSULoadUnitNoLockStats : public statistics::Group
    {
      statistics::Scalar full_forward;
      statistics::Scalar partial_forward;
      statistics::Scalar no_forward;

      LSULoadUnitNoLockStats(BaseCPU &cpu):
        statistics::Group(&cpu, "lsu"),
        ADD_STAT(full_forward, ""),
        ADD_STAT(partial_forward, ""),
        ADD_STAT(no_forward, ""){}
    } stats;

  public:
    LSULoadUnitNoLock(const std::string &name,
                LSUStoreUnit *su_,
                Cva6CPU &cpu_) :
        Named(name), su(su_), cpu(cpu_), stats(cpu) { }

  bool canPush(Cva6DynInstPtr inst) { return !cpu.dcache->isBlocked(); }
  void push(Cva6DynInstPtr inst);
  bool canPop(Cva6DynInstPtr inst){ return inst->dreq->isCompleted(); }
  void pop(Cva6DynInstPtr inst){ return; /* Nothing to do */ }
  void flushfrom(Cva6DynInstPtr inst){/* Nothing to do */ }
  bool advance(){ /* Nothing to do */  return false; }
};

class LSUBase
    : public Named, public virtual ReadyValidIntf, public MatchAddrIntf
{
  protected:
    Cva6CPU &cpu;
    LSUStoreUnit store_unit;
    LSULoadUnitNoLock load_unit;
    Cva6DynInstChunk lsu_fifo; /** Lsu bypass buffer */

  public:
    LSUBase(const std::string &name,
            Cva6CPU &cpu_,
            const BaseCva6CPUParams &p) :
        Named(name),
        cpu(cpu_),
        store_unit(name + ".store_unit", cpu, p),
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

    bool isMaskMatchVaddr(Cva6DynInstPtr inst, uint64_t mask) override {
      /* First check lsu_fifo */
      /* Used only in DUAL LSU */
      for (auto &i2: lsu_fifo){
        if (i2->isAfterOrEqual(inst)){ /* Ignore past instructions */
          continue;
        }
        if (i2->isFault()){ /* Stall if fault before */
          return true;
        }
        if (i2->staticInst->isLoad()){ /* Ignore loads */
          continue;
        }
        if (maskMatchVaddrInst(inst, i2, mask)){ /* */
          return true;
        }
      }
      return store_unit.isMaskMatchVaddr(inst, mask);
    }
};

class LSUBaseChecker
    : public Named, public virtual ReadyValidIntf
{
  protected:
    Cva6CPU &cpu;
    Cva6DynInstChunk cqueue;  /** CheckQueue */
    Cva6DynInstChunk lsu_fifo; /** Lsu bypass buffer */

    /* Pointer to the stores buffer */
    MatchAddrIntf *su;
  public:
    LSUBaseChecker(const std::string &name,
            Cva6CPU &cpu_,
            const BaseCva6CPUParams &params,
            MatchAddrIntf *su_) :
        Named(name),
        cpu(cpu_),
        cqueue(name + ".CQ"),
        lsu_fifo(name + ".LSQ"),
        su(su_)
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
            checker(name, cpu_, params, &base) {}
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


// LSUWithCheckerLQ

class FULSU
  : public LSUBase, public FUBase
{
  public:
    FULSU(const std::string &name,
          std::vector<OpClass> &ops,
          Cva6CPU &cpu_,
          const BaseCva6CPUParams &params)
      : LSUBase(name, cpu_, params),
        FUBase(name, ops)
        { ; }
};


} // namespace cva6
} // namespace gem5
