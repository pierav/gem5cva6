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
  bool getregs = false;

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
          if (getregs){
            rfsrc_set_val(reg, inst->getSrcRegOperand(i));
          }
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
        if (getregs){
          rfdst_set_val(reg, inst->getDstRegOperand(i));
        }
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

  bool isKto0(){
    return rfdst.popcount() == 0;
  }

  bool isRdSingle(){
    return rfdst.isSingle();
  }

  RegId getSingle(){
    assert(isKto1());
    if (isKto0()){
      return i2id(0);
    }
    return rfdst.getSingle();
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
    register_dst_val(cpu) { getregs = true; }

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



class LambdaCheckInst : public StaticInst
{
  uint64_t check;
  RegId srcRegIdxArr[1]; RegId destRegIdxArr[0];
  public:
  LambdaCheckInst(RegId reg, uint64_t v)
    : StaticInst("lambda.check", IntAluOp), check(v) {
    setRegIdxArrays(
      reinterpret_cast<RegIdArrayPtr>(
          &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
      reinterpret_cast<RegIdArrayPtr>(
          &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
    setSrcRegIdx(_numSrcRegs++, reg);
    flags[IsInteger] = true;
    // flags[IsMicroop] = true;
  }
  Fault execute(ExecContext *xc, trace::InstRecord *td) const override {
    uint64_t val = xc->getRegOperand(this, 0);
    if (check != val){
      return std::make_shared<SpeculativeFault>("missprediction");
    }
    return NoFault;
  }
  void advancePC(PCStateBase &pc_state) const override { /* Nothing */}
  std::string
  generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const {
    std::stringstream ss;
    ss << mnemonic << ' ';
    ss << registerName(srcRegIdxArr[0]) << ", ";
    ss << check;
    return ss.str();
  }
};


class LambdaPredInst : public StaticInst
{
  uint64_t value;
  RegId srcRegIdxArr[0]; RegId destRegIdxArr[1];
  public:
  LambdaPredInst(RegId reg, uint64_t v)
    : StaticInst("lambda.pred", IntAluOp), value(v) {
    setRegIdxArrays(
      reinterpret_cast<RegIdArrayPtr>(
          &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
      reinterpret_cast<RegIdArrayPtr>(
          &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
    setDestRegIdx(_numDestRegs++, reg);
    flags[IsInteger] = true;
    // flags[IsMicroop] = true;
  }
  Fault execute(ExecContext *xc, trace::InstRecord *td) const override {
    xc->setRegOperand(this, 0, value);
    return NoFault;
  }
  void advancePC(PCStateBase &pc_state) const override { /* Nothing */}
  std::string
  generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const {
    std::stringstream ss;
    ss << mnemonic << ' ';
    ss << registerName(destRegIdxArr[0]) << ", ";
    ss << value;
    return ss.str();
  }
};


} // namespace cva6
} // namespace gem5
