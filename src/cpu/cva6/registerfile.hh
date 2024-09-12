#pragma once

#include <bits/stdc++.h>

#include "arch/riscv/utility.hh"
#include "cpu/cva6/buffers.hh"
#include "cpu/cva6/cpu.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/reg_class.hh"

namespace gem5 {

/* Some utilities */
std::string riscvRegisterName(RegId reg);
bool reverseRegisterName(Cva6CPU &cpu, char* name, RegId &reg);
int id2i(RegId reg);
RegId i2id(int i);

inline std::string registerName(int i) {
    return riscvRegisterName(i2id(i));
}

/* BinaryRegisterFile: riscv only */
class BinaryRegisterFile
{
  protected:
    uint64_t bitset = 0;

  public:
    BinaryRegisterFile(uint64_t val=0) : bitset(val) {}

  public:
    /* Setters */
    bool isSet(RegId reg){ return (bitset >> id2i(reg)) & 1; }
    bool isSetRaw(int i){ return (bitset >> i) & 1; }
    void set(RegId reg){ bitset |= (uint64_t)1 << id2i(reg); }
    /* Clear */
    void clear(RegId reg){ bitset &= ~((uint64_t)1 << id2i(reg)); }
    void clear() { bitset = 0; }
    void clearall() {  bitset = 0; }
    /* Getters */
    uint64_t get(){ return bitset; }
    uint8_t popcount() { return __builtin_popcountll(bitset); }
    bool isSingle() { return popcount() == 1; }
    RegId getSingle() {
        assert(isSingle());
        return i2id(__builtin_ctzll(bitset));
    }

    std::string dump(const uint64_t *vals=NULL);
};

template <class T>
class RegisterFile
{

  protected:

    BaseCPU &cpu;
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

    RegisterFile(BaseCPU &cpu_) :
        cpu(cpu_),
        regClasses(cpu.getContext(0)->getIsaPtr()->regClasses()),
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

} // namespace gem5
