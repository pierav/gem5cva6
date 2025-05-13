/**
 * @file exec_context_static.hh
 * @author Pierre Ravenel (pravenel@kalrayinc.com)
 * @brief ExecContextStatic : Extract static info after decode
 * @version 1.0
 * @date 2023-05-25
 */

#pragma once

#include "cpu/base.hh"
#include "cpu/exec_context.hh"

namespace gem5 {
namespace cva6 {

struct StaticData
{
    uint16_t mem_req_size = 0;
    uint64_t mem_req_imm = 0;
    StaticData() {}
};

class ExecContextStatic : public gem5::ExecContext
{
    StaticData &data;
    const GenericISA::SimplePCState<1> pc;

  public:
    ExecContextStatic(StaticData &data_) :
        data(data_) { }

    ~ExecContextStatic() { }

    Fault
    initiateMemRead(Addr addr, unsigned int size,
                    Request::Flags flags,
                    const std::vector<bool>& byte_enable){
        data.mem_req_size = size;
        data.mem_req_imm = addr;
        return NoFault;
    }

    Fault
    initiateMemMgmtCmd(Request::Flags flags) override {
        return NoFault;
    }

    Fault
    writeMem(uint8_t *data_, unsigned int size, Addr addr,
             Request::Flags flags, uint64_t *res,
             const std::vector<bool>& byte_enable) override {
        data.mem_req_size = size;
        data.mem_req_imm = addr;
        return NoFault;
    }

    Fault
    initiateMemAMO(Addr addr, unsigned int size, Request::Flags flags,
                   AtomicOpFunctorPtr amo_op) override {
        data.mem_req_size = size;
        data.mem_req_imm = addr;
        return NoFault;
    }

    RegVal
    getRegOperand(const StaticInst *si, int idx) override {
        return 0;
    }

    void
    getRegOperand(const StaticInst *si, int idx, void *val) override {}

    void *
    getWritableRegOperand(const StaticInst *si, int idx) override {
        return NULL;
    }

    void
    setRegOperand(const StaticInst *si, int idx, RegVal val) override {}

    void
    setRegOperand(const StaticInst *si, int idx, const void *val) override {}

    bool
    readPredicate() const override { return false; }

    void
    setPredicate(bool val) override {}

    bool
    readMemAccPredicate() const override { return false; }

    void
    setMemAccPredicate(bool val) override {}

    uint64_t
    getHtmTransactionUid() const override { return 0;}

    uint64_t
    newHtmTransactionUid() const override { return 0; }

    bool
    inHtmTransactionalState() const override { return false; }

    uint64_t
    getHtmTransactionalDepth() const override { return 0; }

    const PCStateBase &
    pcState() const override { return pc; }

    void
    pcState(const PCStateBase &val) override {}

    RegVal
    readMiscRegNoEffect(int misc_reg) const { return 0; }

    RegVal
    readMiscReg(int misc_reg) override { return 0; }

    void
    setMiscReg(int misc_reg, RegVal val) override { }

    RegVal
    readMiscRegOperand(const StaticInst *si, int idx) override { return 0; }

    void
    setMiscRegOperand(const StaticInst *si, int idx, RegVal val) override {}

    ThreadContext *tcBase() const override { return nullptr; }

    unsigned int readStCondFailures() const override { return 0; }

    void setStCondFailures(unsigned int st_cond_failures) override {}

    void
    demapPage(Addr vaddr, uint64_t asn) override { }

    void
    armMonitor(Addr address) override { }

    bool
    mwait(PacketPtr pkt) { return false; }

    void
    mwaitAtomic(ThreadContext *tc) override { }

    AddressMonitor *
    getAddrMonitor() override { return NULL; }
};

} // namespace cva6
} // namespace gem5
