/**
 * @file
 *
 *  Execute function unit descriptions and pipeline implementations.
 */

#ifndef __CPU_CVA6_FUNC_UNIT_HH__
#define __CPU_CVA6_FUNC_UNIT_HH__


#include <cstdint>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <typeinfo>
#include <vector>

#include "base/trace.hh"
#include "base/types.hh"
#include "cpu/cva6/buffers.hh"
#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/fu_base.hh"
#include "cpu/cva6/fu_lsu.hh"
#include "cpu/func_unit.hh"
#include "cpu/timing_expr.hh"
#include "debug/Cva6FU.hh"
#include "sim/clocked_object.hh"
#include "sim/sim_object.hh"

namespace gem5 {
namespace cva6 {

/** Fxed latency unit */
class FUPipeline : public FUBase
{
  protected:
    /* internal pipeline */
    size_t pipe_size;
    Cva6DynInstChunk **pipe; // TODO ring buffer
    bool is_pipeline;

  public:
    FUPipeline(const std::string &name_, std::vector<OpClass> &ops,
               size_t lat, bool is_pipeline_=true) :
      FUBase(name_, ops),
      pipe_size(lat + 1), // + 1 for latch
      pipe(new Cva6DynInstChunk*[pipe_size]),
      is_pipeline(is_pipeline_) {
        for (int i = 0; i < pipe_size; i++){
          pipe[i] = new Cva6DynInstChunk(name_ + ".pipe");
        }
        // flushfrom();
      }

    ~FUPipeline(){
      delete pipe;
    }

    bool canPush(Cva6DynInstPtr inst){
      if (!pipe[0]->canPush(inst) ||      /** Input slot not free */
          (!is_pipeline && occ() != 0) /** Pipeline not free */
        ){
        DPRINTF(Cva6FU, "Can't issue into FU: busy (occ = %ld)\n", occ());
        return false;
      }
      return true;
    }

    void push(Cva6DynInstPtr inst){
      assert(canPush(inst));
      pipe[0]->push(inst);
    }

    bool advance(void){
      if (pipe[pipe_size-1]->empty() /** Not stalled */){
        /** Advance pipe if needed */
        Cva6DynInstChunk *tmp = pipe[pipe_size-1];
        if (occ()){ /** Avoid unused memory moves */
          for (int i = pipe_size - 1; i > 0; i --){
            pipe[i] = pipe[i-1];
          }
        }
        pipe[0] = tmp;
      }
      return occ();
    }

    bool canPop(Cva6DynInstPtr inst){
      return pipe[pipe_size-1]->canPop(inst);
    }

    void pop(Cva6DynInstPtr inst){
      // assert(canPop(inst));
      pipe[pipe_size-1]->pop(inst);
    }

    void flushfrom(Cva6DynInstPtr inst_){
      for (size_t i = 0; i < pipe_size; i++){
        if (!pipe[i]->empty()){
          pipe[i]->flushfrom(inst_);
        }
      }
    }

    size_t occ(){
      size_t ret = 0;
      for (size_t i = 0; i < pipe_size; i++){
        if (!pipe[i]->empty()){
          ret += 1;
        }
      }
      return ret;
    }
};

class FUNoCost : public FUBase
{
  public:
    FUNoCost(const std::string &name_, std::vector<OpClass> &ops)
      : FUBase(name_, ops) {}
    bool canPush(Cva6DynInstPtr inst) { return true; };
    void push(Cva6DynInstPtr inst) { };
    bool advance(void) { return false; };
    bool canPop(Cva6DynInstPtr inst) { return true; }
    void pop(Cva6DynInstPtr inst) { };
    void flushfrom(Cva6DynInstPtr inst) { };
};


class FUUnimp : public FUBase
{
  public:
    FUUnimp(const std::string &name_, std::vector<OpClass> &ops)
      : FUBase(name_, ops) {}
    bool canPush(Cva6DynInstPtr inst) {
      fatal("Try to push invalid inst: %s", *inst);
      return false;
    };
    void push(Cva6DynInstPtr inst) { };
    bool advance(void) { return false; };
    bool canPop(Cva6DynInstPtr inst) {return true; }
    void pop(Cva6DynInstPtr inst) { };
    void flushfrom(Cva6DynInstPtr inst) { };
};


class FUPipelines : public Named
{
  private:
    Cva6CPU &cpu;           /** Pointer back to cpu */

    /* Fonctional Units */
    FUBase *alu;            /** Arithmetic logic unit */
    FUBase *mul;            /** Multiplier */
    FUBase *serdiv;         /** Serial divider */
    FUBase *fpu;            /** Floating point unit */
    FUBase *fpu_divsqrt;    /** Floating point unit */
    FUBase *simd;           /** SIMD Unit*/
    FUBase *lsu;            /** Load Store Unit */
    FUBase *misc;           /** Not used */
    FUBase *nocost;         /** Not used */

    /* List containing all FUS */
    std::vector<FUBase *> funcUnits;

  public:

    FUPipelines(const std::string &name,
                Cva6CPU &cpu_,
                const BaseCva6CPUParams &params):
      Named(name),
      cpu(cpu_)
    {
      std::vector<OpClass> noop_set = { OpClass::No_OpClass };
      std::vector<OpClass> alu_set =  { OpClass::IntAlu };
      std::vector<OpClass> mul_set =  { OpClass::IntMult };
      std::vector<OpClass> div_set =  { OpClass::IntDiv };
      std::vector<OpClass> fpu_set =  { OpClass::FloatAdd,
                                        OpClass::FloatCmp,
                                        OpClass::FloatCvt,
                                        OpClass::FloatMult,
                                        OpClass::FloatMultAcc,
                                        OpClass::FloatMisc };
      std::vector<OpClass> fpu2_set = { OpClass::FloatDiv,
                                        OpClass::FloatSqrt };
      std::vector<OpClass> simd_set = { OpClass::SimdAdd,
                                        OpClass::SimdAddAcc,
                                        OpClass::SimdAlu,
                                        OpClass::SimdCmp,
                                        OpClass::SimdCvt,
                                        OpClass::SimdMisc,
                                        OpClass::SimdMult,
                                        OpClass::SimdMultAcc,
                                        OpClass::SimdMatMultAcc,
                                        OpClass::SimdShift,
                                        OpClass::SimdShiftAcc,
                                        OpClass::SimdDiv,
                                        OpClass::SimdSqrt,
                                        OpClass::SimdFloatAdd,
                                        OpClass::SimdFloatAlu,
                                        OpClass::SimdFloatCmp,
                                        OpClass::SimdFloatCvt,
                                        OpClass::SimdFloatDiv,
                                        OpClass::SimdFloatMisc,
                                        OpClass::SimdFloatMult,
                                        OpClass::SimdFloatMultAcc,
                                        OpClass::SimdFloatMatMultAcc,
                                        OpClass::SimdFloatSqrt,
                                        OpClass::SimdReduceAdd,
                                        OpClass::SimdReduceAlu,
                                        OpClass::SimdReduceCmp,
                                        OpClass::SimdFloatReduceAdd,
                                        OpClass::SimdFloatReduceCmp,
                                        OpClass::SimdAes,
                                        OpClass::SimdAesMix,
                                        OpClass::SimdSha1Hash,
                                        OpClass::SimdSha1Hash2,
                                        OpClass::SimdSha256Hash,
                                        OpClass::SimdSha256Hash2,
                                        OpClass::SimdShaSigma2,
                                        OpClass::SimdShaSigma3,
                                        OpClass::SimdShaSigma3,
                                        OpClass::Matrix,
                                        OpClass::MatrixMov,
                                        OpClass::MatrixOP,
                                        OpClass::SimdPredAlu };
      std::vector<OpClass> lsu_set =  { OpClass::MemRead,
                                        OpClass::MemWrite,
                                        OpClass::FloatMemRead,
                                        OpClass::FloatMemWrite };
      std::vector<OpClass> misc_set = { OpClass::IprAccess,
                                        OpClass::InstPrefetch };
      std::vector<OpClass> vec_set =  { OpClass::VectorUnitStrideLoad,
                                        OpClass::VectorUnitStrideStore,
                                        OpClass::VectorUnitStrideMaskLoad,
                                        OpClass::VectorUnitStrideMaskStore,
                                        OpClass::VectorStridedLoad,
                                        OpClass::VectorStridedStore,
                                        OpClass::VectorIndexedLoad,
                                        OpClass::VectorIndexedStore,
                                  OpClass::VectorUnitStrideFaultOnlyFirstLoad,
                                        OpClass::VectorWholeRegisterLoad,
                                        OpClass::VectorWholeRegisterStore,
                                        OpClass::VectorIntegerArith,
                                        OpClass::VectorFloatArith,
                                        OpClass::VectorFloatConvert,
                                        OpClass::VectorIntegerReduce,
                                        OpClass::VectorFloatReduce,
                                        OpClass::VectorMisc,
                                        OpClass::VectorIntegerExtension,
                                        OpClass::VectorConfig };
      // No operations FU
      nocost = new FUNoCost(name + ".nocost", noop_set);

      // Operations mapping
      alu = new FUPipeline(name + ".alu", alu_set, 1);
      mul = new FUPipeline(name + ".mul", mul_set, 3);
      serdiv = new FUPipeline(name + ".div", div_set, 8, false);
      // TODO variable latency serdiv
      fpu = new FUPipeline(name + ".fpu", fpu_set, 3);
      // TODO: Fusion and &  variable latency !!
      fpu_divsqrt = new FUPipeline(name + ".fpudiv", fpu2_set,18, false);
      simd = new FUPipeline(name + ".simd", simd_set, 4);
      lsu = new FULSU(name + ".lsu", lsu_set, cpu, params);
      misc = new FUPipeline(name + ".misc", misc_set, 1);
      FUBase *fuunimp = new FUUnimp(name + ".unimp", vec_set);

      funcUnits = {nocost, alu, mul, serdiv, fpu,
                  fpu_divsqrt, simd, lsu, misc, fuunimp};

      /** Check that there is a functional unit for all operation classes */
      for (int iop = No_OpClass + 1; iop < Num_OpClasses; iop++) {
        bool hit = false;
        for (FUBase *fu : funcUnits) {
            hit |= fu->provides(static_cast<OpClass>(iop));
        }
        panic_if(!hit, "No FU for OpClass %s\n", enums::OpClassStrings[iop]);
      }
    }

    /** Return the number of functional unit */
    size_t nbFu(){
      return funcUnits.size();
    }

    int capableFuIndex(Cva6DynInstPtr inst){
      if (inst->isFault()){ // nocost FU
        return 0;
      }
      for (int i = 0; i < funcUnits.size(); i++){
        if (funcUnits[i]->provides(inst->staticInst->opClass())){
          return i;
        }
      }
      assert(0); // Fu must exist
      return -1;
    }

    int getValidFuIndex(Cva6DynInstPtr inst){
      int fu_index = capableFuIndex(inst);
      int res = funcUnits[fu_index]->canPush(inst) ? fu_index : -1;
      // DPRINTF(Cva6FU, "Inst %s should go in FU %d\n", *inst, fu_index);
      // DPRINTF(Cva6FU, "--- FU %d is (%s)\n", fu_index,
      //         res == -1 ? "BUSY" : "READY");
      return res;
    }

    void debug(void){
      DPRINTF(Cva6FU, "------------- FUS #%d---------------\n",
         funcUnits.size());
      for (int i = 0; i < funcUnits.size(); i++){
        DPRINTF(Cva6FU, "FU%d is: %s\n", i, funcUnits[i]->dump().c_str());
      }
    }

    /* ReadyValidIntf interface */
    bool canPush(Cva6DynInstPtr inst){
      // if (inst->isFault()){ // We can always push fault
      //   return true;
      // }
      return getValidFuIndex(inst) != -1;
    }

    void push(Cva6DynInstPtr inst){
      // if (inst->isFault()) {
      //   return; // Nothing to do
      // }
      DPRINTF(Cva6FU, "push %s in fu%d \n", *inst, inst->fuIndex);
      funcUnits[inst->fuIndex]->push(inst);
    }

    bool advance(void){
      bool ret = false;
      DPRINTF(Cva6FU, "Advance FUS\n");
      /* Advance the pipelines and note whether they still need to be
      * advanced */
      for (FUBase *fu : funcUnits) {
        ret |= fu->advance();
      }
      return ret;
    }

    bool canPop(Cva6DynInstPtr inst){
      bool ret = funcUnits[inst->fuIndex]->canPop(inst);
      DPRINTF(Cva6FU, "Inst %s %s in FU %d\n", *inst,
        ret ? "ready" : "buzy", inst->fuIndex);
      return ret;
    }

    void pop(Cva6DynInstPtr inst){
      DPRINTF(Cva6FU, "pop %s in fu%d \n", *inst, inst->fuIndex);
      funcUnits[inst->fuIndex]->pop(inst);
    }

    void flushfrom(Cva6DynInstPtr inst){
      DPRINTF(Cva6FU, "Flush FUS\n");
      for (FUBase *fu : funcUnits) {
        fu->flushfrom(inst);
      }
    }
};


} // namespace cva6
} // namespace gem5

#endif /* __CPU_CVA6_FUNC_UNIT_HH__ */
