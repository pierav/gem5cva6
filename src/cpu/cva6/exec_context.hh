/*
 * Copyright (c) 2011-2014, 2016-2018, 2020-2021 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2002-2005 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *
 *  ExecContext bears the exec_context interface for Cva6.
 */

#ifndef __CPU_CVA6_EXEC_CONTEXT_HH__
#define __CPU_CVA6_EXEC_CONTEXT_HH__

#include "cpu/base.hh"
#include "cpu/cva6/pipeline.hh"
#include "cpu/exec_context.hh"
#include "cpu/simple_thread.hh"
#include "debug/Cva6Execute.hh"
#include "mem/request.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Cva6, cva6);
namespace cva6
{

/* Forward declaration of Execute */
class Execute;

/** ExecContext bears the exec_context interface for Cva6.  This nicely
 *  separates that interface from other classes such as Pipeline, Cva6CPU
 *  and DynCva6Inst and makes it easier to see what state is accessed by it.
 */
class ExecContext : public gem5::ExecContext
{
  public:
    Cva6CPU &cpu;

    /** ThreadState object, provides all the architectural state. */
    SimpleThread &thread;

    /** Instruction for the benefit of memory operations and for PC */
    Cva6DynInstPtr inst;

    ExecContext (
        Cva6CPU &cpu_,
        SimpleThread &thread_,
        Cva6DynInstPtr inst_) :
        cpu(cpu_),
        thread(thread_),
        inst(inst_)
    {
        DPRINTF(Cva6Execute, "ExecContext setting PC: %s\n", *inst->pc);
        pcState(*inst->pc);
        setPredicate(inst->readPredicate());
        setMemAccPredicate(inst->readMemAccPredicate());
    }

    ~ExecContext()
    {
        inst->setPredicate(readPredicate());
        inst->setMemAccPredicate(readMemAccPredicate());
    }


    Fault
    initiateMemRead(Addr addr, unsigned int size,
                    Request::Flags flags,
                    const std::vector<bool>& byte_enable) override {
        panic("Unimplemented\n");
        return NoFault;
    }

    Fault
    initiateMemMgmtCmd(Request::Flags flags) override {
        panic("Unimplemented\n");
        return NoFault;
    }

    Fault
    writeMem(uint8_t *data, unsigned int size, Addr addr,
             Request::Flags flags, uint64_t *res,
             const std::vector<bool>& byte_enable) override {
        panic("Unimplemented\n");
        return NoFault;
    }

    Fault
    initiateMemAMO(Addr addr, unsigned int size, Request::Flags flags,
                   AtomicOpFunctorPtr amo_op) override {
        panic("Unimplemented\n");
        return NoFault;
    }


    RegVal
    getRegOperand(const StaticInst *si, int idx) override
    {
        const RegId &reg = si->srcRegIdx(idx);
        if (reg.is(InvalidRegClass))
            return 0;
        return thread.getReg(reg);
    }

    void
    getRegOperand(const StaticInst *si, int idx, void *val) override
    {
        thread.getReg(si->srcRegIdx(idx), val);
    }

    void *
    getWritableRegOperand(const StaticInst *si, int idx) override
    {
        return thread.getWritableReg(si->destRegIdx(idx));
    }

    void
    setRegOperand(const StaticInst *si, int idx, RegVal val) override
    {
        const RegId &reg = si->destRegIdx(idx);
        if (reg.is(InvalidRegClass))
            return;
        thread.setReg(si->destRegIdx(idx), val);
        // Also copy values in instructions dst regs
        inst->setDstRegOperand(idx, val);
    }

    void
    setRegOperand(const StaticInst *si, int idx, const void *val) override
    {
        fatal("Do not call!\n");
        thread.setReg(si->destRegIdx(idx), val);
    }

    bool
    readPredicate() const override
    {
        return thread.readPredicate();
    }

    void
    setPredicate(bool val) override
    {
        thread.setPredicate(val);
    }

    bool
    readMemAccPredicate() const override
    {
        return thread.readMemAccPredicate();
    }

    void
    setMemAccPredicate(bool val) override
    {
        thread.setMemAccPredicate(val);
    }

    // hardware transactional memory
    uint64_t
    getHtmTransactionUid() const override
    {
        panic("ExecContext::getHtmTransactionUid() not"
              "implemented on Cva6CPU\n");
        return 0;
    }

    uint64_t
    newHtmTransactionUid() const override
    {
        panic("ExecContext::newHtmTransactionUid() not"
              "implemented on Cva6CPU\n");
        return 0;
    }

    bool
    inHtmTransactionalState() const override
    {
        // ExecContext::inHtmTransactionalState() not
        // implemented on Cva6CPU
        return false;
    }

    uint64_t
    getHtmTransactionalDepth() const override
    {
        panic("ExecContext::getHtmTransactionalDepth() not"
              "implemented on Cva6CPU\n");
        return 0;
    }

    const PCStateBase &
    pcState() const override
    {
        return thread.pcState(); // <=> inst->pcState(); TODO ?
    }

    void
    pcState(const PCStateBase &val) override
    {
        thread.pcState(val);
        // set inst PC state !
        inst->pcState(val);

    }

    RegVal
    readMiscRegNoEffect(int misc_reg) const
    {
        return thread.readMiscRegNoEffect(misc_reg);
    }

    RegVal
    readMiscReg(int misc_reg) override
    {
        return thread.readMiscReg(misc_reg);
    }

    void
    setMiscReg(int misc_reg, RegVal val) override
    {
        thread.setMiscReg(misc_reg, val);
    }

    RegVal
    readMiscRegOperand(const StaticInst *si, int idx) override
    {
        const RegId& reg = si->srcRegIdx(idx);
        assert(reg.is(MiscRegClass));
        return thread.readMiscReg(reg.index());
    }

    void
    setMiscRegOperand(const StaticInst *si, int idx, RegVal val) override
    {
        const RegId& reg = si->destRegIdx(idx);
        assert(reg.is(MiscRegClass));
        return thread.setMiscReg(reg.index(), val);
    }

    ThreadContext *tcBase() const override { return thread.getTC(); }

    /* @todo, should make stCondFailures persistent somewhere */
    unsigned int readStCondFailures() const override { return 0; }
    void setStCondFailures(unsigned int st_cond_failures) override {}

    ContextID contextId() { return thread.contextId(); }
    /* ISA-specific (or at least currently ISA singleton) functions */

    /* X86: TLB twiddling */
    void
    demapPage(Addr vaddr, uint64_t asn) override
    {
        thread.getMMUPtr()->demapPage(vaddr, asn);
    }

    BaseCPU *getCpuPtr() { return &cpu; }

  public:
    // monitor/mwait funtions
    void
    armMonitor(Addr address) override
    {
        getCpuPtr()->armMonitor(0, address);
    }

    bool
    mwait(PacketPtr pkt) override
    {
        return getCpuPtr()->mwait(0, pkt);
    }

    void
    mwaitAtomic(ThreadContext *tc) override
    {
        return getCpuPtr()->mwaitAtomic(0, tc, thread.mmu);
    }

    AddressMonitor *
    getAddrMonitor() override
    {
        return getCpuPtr()->getCpuAddrMonitor(0);
    }
};

} // namespace cva6
} // namespace gem5

#endif /* __CPU_CVA6_EXEC_CONTEXT_HH__ */
