/**
 * lambda_utils.hh
 *
 *  Author:   Pierre Ravenel
 * Created:   19/09/2024
 **/

#pragma once

#include <stdint.h>

#include <cstring>

#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/registerfile.hh"

namespace gem5 {
namespace cva6 {

class BinaryLambdaRegFile
{
  protected:
  BinaryRegisterFile rfsrc;
  BinaryRegisterFile rfdst;

  public:
  BinaryLambdaRegFile() {};

  virtual void rfsrc_set_val(RegId reg, uint64_t value) {}
  virtual void rfdst_set_val(RegId reg, uint64_t value) {}

  void push(Cva6DynInstPtr inst) {
    if (inst->isFault()){ /* Handle fault in predictions */
      return; /* Simply sidestep here */
    }
    // Append all src regs
    for (unsigned int i = 0; i < inst->staticInst->numSrcRegs(); i++) {
      RegId reg = inst->staticInst->srcRegIdx(i);
      if (reg.classValue() != InvalidRegClass){
        if (!rfdst.isSet(reg)){ /* Is register lambda input */
          rfsrc.set(reg);
          rfsrc_set_val(reg, inst->getSrcRegOperand(i));
        }
        /* Perform reg dead check */
        if (inst->exec_data.is_reg_dead[i]){
          rfdst.clear(reg);
        }
      }
    }
    /* Append all destination registers */
    for (unsigned int i = 0; i < inst->staticInst->numDestRegs(); i++) {
      RegId reg = inst->staticInst->destRegIdx(i);
      if (reg.classValue() != InvalidRegClass){
        rfdst.set(reg);
        rfdst_set_val(reg, inst->getDstRegOperand(i));
      }
    }
  }

  void clear(){
    rfsrc.clear();
    rfdst.clear();
  }

  void isXtoY(int x, int y){
    fatal("Unimplemented!");
  }

  bool isKto1(){
    return rfdst.popcount() <= 1;
  }

  bool isRdSingle(){
    return rfdst.isSingle();
  }

  std::string dump(){
    std::stringstream ss;
    ss << "L:" << rfsrc.dump() << "->" << rfdst.dump();
    return ss.str();
  }
};

class LambdaRegFile : public BinaryLambdaRegFile
{
  RegisterFile<uint64_t> register_src_val;
  RegisterFile<uint64_t> register_dst_val;
  public:
  LambdaRegFile(BaseCPU &cpu) :
    BinaryLambdaRegFile(),
    register_src_val(cpu),
    register_dst_val(cpu) { }

  void rfsrc_set_val(RegId reg, uint64_t value) override {
    register_src_val.set(reg, value);
  }
  void rfdst_set_val(RegId reg, uint64_t value) override {
    register_dst_val.set(reg, value);
  }
  std::pair<RegId, uint64_t> getSingle(){
    assert(isKto1());
    RegId reg = rfdst.getSingle();
    uint64_t val = register_dst_val.get(reg);
    return {reg, val};
  }
};

} // namespace cva6
} // namespace gem5
