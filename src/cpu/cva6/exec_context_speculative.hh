/**
 * @file
 *
 *  ExecContextSpeculative: execute speculatively inst
 */

#pragma once

#include "cpu/base.hh"
#include "cpu/exec_context.hh"

namespace gem5 {
namespace cva6 {

class ExecContextSpeculative : public gem5::ExecContext
{

    Cva6DynInstPtr inst;

  public:
    ExecContextSpeculative(Cva6DynInstPtr inst_) : inst(inst_) { }

    ~ExecContextSpeculative() { }

    Fault
    initiateMemRead(Addr addr, unsigned int size,
                    Request::Flags flags,
                    const std::vector<bool>& byte_enable)
        override {
        return inst->initiateMemRead(addr, size, flags, byte_enable);
    }

    Fault
    initiateMemMgmtCmd(Request::Flags flags) override {
        panic("ExecContextSpeculative::initiateMemMgmtCmd() not implemented "
              " on Cva6CPU\n");
        return NoFault;
    }

    Fault
    writeMem(uint8_t *data, unsigned int size, Addr addr,
             Request::Flags flags, uint64_t *res,
             const std::vector<bool>& byte_enable) override {
        return inst->writeMem(data, size, addr, flags, res, byte_enable);
    }

    Fault
    initiateMemAMO(Addr addr, unsigned int size, Request::Flags flags,
                   AtomicOpFunctorPtr amo_op) override {
        return inst->initiateMemAMO(addr, size, flags,
            AtomicOpFunctorPtr(amo_op->clone()));
    }

    RegVal
    getRegOperand(const StaticInst *si, int idx) override {
        // std::cout << "getRegOperand " << *inst << " " << idx << std::endl;
        return inst->getSrcRegOperand(idx);
    }

    void
    getRegOperand(const StaticInst *si, int idx, void *val) override {
        panic("Unimplemented\n");
    }

    void *
    getWritableRegOperand(const StaticInst *si, int idx) override {
        panic("Unimplemented\n");
    }

    void
    setRegOperand(const StaticInst *si, int idx, RegVal val) override {
        // std::cout << "setRegOperand " << *inst << " " << idx << std::endl;
        inst->setDstRegOperand(idx, val);
    }

    void
    setRegOperand(const StaticInst *si, int idx, const void *val) override {
        panic("TODO\n");
    }

    bool
    readPredicate() const override {
        panic("Unimplemented\n");
    }

    void
    setPredicate(bool val) override {
        panic("Unimplemented\n");
    }

    bool
    readMemAccPredicate() const override {
        panic("Unimplemented\n");
        return 0;
    }

    void
    setMemAccPredicate(bool val) override {
        panic("Unimplemented\n");
    }

    // hardware transactional memory
    uint64_t
    getHtmTransactionUid() const override {
        panic("Unimplemented\n");
        return 0;
    }

    uint64_t
    newHtmTransactionUid() const override
    {
       panic("Unimplemented\n");
        return 0;
    }

    bool
    inHtmTransactionalState() const override
    {
        panic("Unimplemented\n");
        return false;
    }

    uint64_t
    getHtmTransactionalDepth() const override
    {
        panic("Unimplemented\n");
        return 0;
    }

    const PCStateBase &
    pcState() const override {
        // std::cout << "pcState GET " << std::endl;
        return inst->pcState();
    }

    void
    pcState(const PCStateBase &val) override {
        // std::cout << "pcState SET " << val << " " << std::endl;
        inst->pcState(val);
    }

    RegVal
    readMiscRegNoEffect(int misc_reg) const {
        return inst->readMiscRegNoEffect(misc_reg);
    }

    RegVal
    readMiscReg(int misc_reg) override {
        return inst->readMiscReg(misc_reg);
    }

    void
    setMiscReg(int misc_reg, RegVal val) override {
        // std::cout << "setMiscReg " << misc_reg << " " << val << std::endl;
        inst->setMiscReg(misc_reg, val);
    }

    RegVal
    readMiscRegOperand(const StaticInst *si, int idx) override {
        panic("readMiscRegOperand cannot be speculative !\n");
    }

    void
    setMiscRegOperand(const StaticInst *si, int idx, RegVal val) override {
        panic("setMiscRegOperand cannot be speculative !\n");
    }

    /* TODO: CARE data leak */
    ThreadContext *tcBase() const override { return inst->tcBase(); }

    // ThreadContext *tcBase() const override {
    //     panic("tcBase cannot be called on %s\n", *inst);
    //     return NULL;
    // }

    unsigned int readStCondFailures() const override {
        panic("Unimplemented\n");
        return 0;
    }

    void setStCondFailures(unsigned int st_cond_failures) override {
        panic("Unimplemented\n");
    }

    void
    demapPage(Addr vaddr, uint64_t asn) override {
        panic("Unimplemented\n");
    }

  public:

    void
    armMonitor(Addr address) override
    {
        panic("Unimplemented\n");
    }

    bool
    mwait(PacketPtr pkt) override
    {
        panic("Unimplemented\n");
        return false;
    }

    void
    mwaitAtomic(ThreadContext *tc) override
    {
        panic("Unimplemented\n");
    }

    AddressMonitor *
    getAddrMonitor() override
    {
        panic("Unimplemented\n");
        return NULL;
    }
};

} // namespace cva6
} // namespace gem5
