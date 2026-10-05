/**
 * cbp2025.hh
 *
 *  Author:   Pierre Ravenel
 * Created:   03/06/2026
 **/
#pragma once

#include <zlib.h>

#include <cstdint>
#include <fstream>
#include <vector>

#include "cpu/cva6/registerfile.hh"
#include "debug/Cva6Plugin.hh"

// Trace Format :
// Inst PC                  - 8 bytes
// Inst Type                - 1 byte
// If load/storeInst
//   Effective Address      - 8 bytes
//   Access Size (total)    - 1 byte
//   Involves Base Update   - 1 byte
//   If Store:
//      Involves Reg Offset - 1 byte
// If branch
//   Taken                  - 1 byte
//   If Taken:
//      Target              - 8 bytes
// Num Input Regs           - 1 byte
// Input Reg Names          - 1 byte each
// Num Output Regs          - 1 byte
// Output Reg Names         - 1 byte each
// Output Reg Values
//   If INT                 - 8 bytes each
//   If SIMD                - 16 bytes each
//

namespace gem5 {
namespace cva6 {

// Luck : riscv and Arch64 have the same mapping ofr integer
// INT registers are registers 0 to 31. SIMD/FP registers are
// registers 32 to 63. Flag register is register 64
inline bool reg_is_int(uint8_t reg_offset)
{
    return reg_offset < 32;
}


struct TraceRecordCBP2025
{

    enum class InstClass : uint8_t
    {
        aluInstClass = 0,
        loadInstClass = 1,
        storeInstClass = 2,
        condBranchInstClass = 3,
        uncondDirectBranchInstClass = 4,
        uncondIndirectBranchInstClass = 5,
        fpInstClass = 6,
        slowAluInstClass = 7,
        undefInstClass = 8,
        callDirectInstClass = 9,
        callIndirectInstClass = 10,
        ReturnInstClass = 11,
    };

    uint64_t pc;
    InstClass instClass;

    // Load/Store
    uint64_t effAddr;
    uint8_t accessSize;
    uint8_t baseUpdate;
    uint8_t regOffset;

    // Branch
    uint8_t taken;
    uint64_t target;

    std::vector<uint8_t> inputRegs;
    std::vector<uint8_t> outputRegs;

    std::vector<uint64_t> intValues;
    std::vector<__uint128_t> simdValues;

    TraceRecordCBP2025(Cva6DynInstPtr inst){
        instClass = InstClass::undefInstClass;
        pc = inst->pc->instAddr();

        if (inst->isFault()){
            return;
        }
        StaticInstPtr si = inst->staticInst;
        instClass = si->isReturn()                             ?
                    InstClass::ReturnInstClass :
                    si->isCall() && si->isIndirectCtrl()       ?
                    InstClass::callIndirectInstClass :
                    si->isCall() && si->isDirectCtrl()         ?
                    InstClass::callDirectInstClass :
                    si->isDirectCtrl() && si->isCondCtrl()     ?
                    InstClass::condBranchInstClass :
                    si->isDirectCtrl() && si->isUncondCtrl()   ?
                    InstClass::uncondDirectBranchInstClass :
                    si->isIndirectCtrl() && si->isUncondCtrl() ?
                    InstClass::uncondIndirectBranchInstClass :
                    si->isStore()                              ?
                    InstClass::storeInstClass :
                    si->isLoad()                               ?
                    InstClass::loadInstClass :
                    si->isFloating()                           ?
                    InstClass::fpInstClass :
                    si->isInteger()                            ?
                    InstClass::aluInstClass :
                    InstClass::undefInstClass;

        if (instClass == InstClass::storeInstClass ||
            instClass == InstClass::loadInstClass){
            effAddr = inst->dreq->req->getVaddr();
            accessSize = inst->dreq->req->getSize();
            baseUpdate = false;
        }

        if (instClass == InstClass::storeInstClass){
            regOffset = false;
        }
        taken = si->isUncondCtrl() ? 1 : inst->pc_next_taken;
        target = inst->pc_next->instAddr();

        for (auto &reg: inst->regs_src_phy){
          inputRegs.push_back(reg.virt_reg_idx);
        }
        for (auto &reg: inst->regs_dst_phy){
            outputRegs.push_back(reg.virt_reg_idx);
            if (reg_is_int(reg.virt_reg_idx)){
                intValues.push_back(reg.value);
            } else {
                intValues.push_back(reg.value);
                intValues.push_back(0);
            }
        }
        fatal_if(inputRegs.size() > 3, "Invalid %s", *inst);
        fatal_if(outputRegs.size() > 2, "Invalid %s", *inst);

    }

    void write(gzFile out){
        // PC
        gzwrite(out, &pc, sizeof(uint64_t));

        // Inst class
        uint8_t inst = static_cast<uint8_t>(instClass);
        gzwrite(out, &inst, sizeof(uint8_t));

        // Load / Store
        if (instClass == InstClass::loadInstClass ||
            instClass == InstClass::storeInstClass)
        {
            gzwrite(out, &effAddr, sizeof(uint64_t));
            gzwrite(out, &accessSize, sizeof(uint8_t));
            gzwrite(out, &baseUpdate, sizeof(uint8_t));

            if (instClass == InstClass::storeInstClass)
            {
                gzwrite(out, &regOffset, sizeof(uint8_t));
            }
        }

        // Branch
        if (instClass == InstClass::condBranchInstClass ||
            instClass == InstClass::uncondDirectBranchInstClass ||
            instClass == InstClass::uncondIndirectBranchInstClass ||
            instClass == InstClass::callDirectInstClass ||
            instClass == InstClass::callIndirectInstClass ||
            instClass == InstClass::ReturnInstClass)
        {
            gzwrite(out, &taken, sizeof(uint8_t));

            if (taken) {
                gzwrite(out, &target, sizeof(uint64_t));
            }
        }

        // Input registers
        uint8_t numInputs = inputRegs.size();
        gzwrite(out, &numInputs, sizeof(uint8_t));

        for (uint8_t r : inputRegs)
            gzwrite(out, &r, sizeof(uint8_t));

        // Output registers
        uint8_t numOutputs = outputRegs.size();
        gzwrite(out, &numOutputs, sizeof(uint8_t));

        for (uint8_t r : outputRegs)
            gzwrite(out, &r, sizeof(uint8_t));

        // Output INT values
        for (auto v : intValues)
            gzwrite(out, &v, sizeof(uint64_t));

        // Output SIMD values
        for (auto v : simdValues)
            gzwrite(out, &v, sizeof(__uint128_t));
    }
};


} // namespace cva6
} // namespace gem5
