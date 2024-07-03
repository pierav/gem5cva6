#pragma once

#include <bits/stdc++.h>

#include "arch/riscv/utility.hh"
#include "cpu/cva6/buffers.hh"
#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/reg_class.hh"

namespace gem5 {
namespace cva6 {

std::string riscvRegisterName(RegId reg);

RegId reverseRegisterName(Cva6CPU &cpu, char* name);


/* BinaryRegisterFile: riscv only */
class BinaryRegisterFile
{
  protected:
    uint64_t bitset = 0;

  public:
        BinaryRegisterFile(uint64_t val=0) : bitset(val) {}

  protected:
    int key(RegId reg){
        if (reg.classValue() == InvalidRegClass){
            return 66; // Out of boud
        } else if (reg.is(IntRegClass)){
            return reg.index();
        } else if (reg.is(FloatRegClass)){
            return reg.index() + 32;
        }
        fatal("Invalid register: %s\n", reg);
    }

  public:
    bool isSet(RegId reg){ return (bitset >> key(reg)) & 1; }
    bool isSetRaw(int i){ return (bitset >> i) & 1; }
    void set(RegId reg){ bitset |= (uint64_t)1 << key(reg); }
    void clear(RegId reg){ bitset &= ~((uint64_t)1 << key(reg)); }
    uint64_t get(){ return bitset; }
    void clearall() {  bitset = 0; }
    uint8_t popcount() { return __builtin_popcount(bitset); }

    // RegId i2id(int i){
    //     return RegId(i < 32 ? IntRegClass : FloatRegClass, i % 32);
    // }
    std::string dump(const uint64_t *vals=NULL);

};

template <class T>
class RegisterFile
{

  protected:

    Cva6CPU &cpu;
    const BaseISA::RegClasses regClasses;

    const unsigned intRegOffset;
    const unsigned floatRegOffset;
    const unsigned ccRegOffset;
    const unsigned vecRegOffset;
    const unsigned vecPredRegOffset;

  public:

    const unsigned numRegs;

    // The register file
    std::vector<T> rf;


    RegisterFile(Cva6CPU &cpu_) :
        cpu(cpu_),
        regClasses(cpu.thread->getIsaPtr()->regClasses()),
        intRegOffset(0),
        floatRegOffset(intRegOffset + regClasses.at(IntRegClass)->numRegs()),
        ccRegOffset(floatRegOffset + regClasses.at(FloatRegClass)->numRegs()),
        vecRegOffset(ccRegOffset + regClasses.at(CCRegClass)->numRegs()),
        vecPredRegOffset(vecRegOffset +
                regClasses.at(VecElemClass)->numRegs()),
        numRegs(vecPredRegOffset + regClasses.at(VecPredRegClass)->numRegs()),
        rf(numRegs)
    { }

    uint16_t index(const RegId& reg){
        switch (reg.classValue()) {
            case IntRegClass:
                return reg.index();
            case FloatRegClass:
                return floatRegOffset + reg.index();
            case VecRegClass:
            case VecElemClass:
                return vecRegOffset + reg.index();
            case VecPredRegClass:
                return vecPredRegOffset + reg.index();
            case CCRegClass:
                return ccRegOffset + reg.index();
            case MiscRegClass:
            case InvalidRegClass:
            default:
                return -1;
                panic("Invalid register class: %d", reg.classValue());
        }
    }

    void set(const RegId& reg, T val){
        if (index(reg) == -1){
            return;
        }
        rf[index(reg)] = val;
    }

    T get(const RegId& reg){
        if (index(reg) == -1){
            panic("Invalid register class: %d", reg.classValue());
        }
        return rf[index(reg)];
    }

    T getraw(uint16_t id){
        return rf[id];
    }



    bool all(T val){
        for (T _val : rf){
            if (_val != val){
                return false;
            }
        }
        return true;
    }

    void setAll(T val){
        for (int i = 0; i < numRegs; i++){
            rf[i] = val;
        }
    }
    uint16_t count(T val){
        uint16_t count = 0;
        for (T _val : rf){
            if (_val == val){
                count ++;
            }
        }
        return count;
    }

};

} // namespace cva6
} // namespace gem5
