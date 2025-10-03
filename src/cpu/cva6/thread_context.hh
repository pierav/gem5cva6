/**
 * @file thread_context.hh
 * @author Pierre Ravenel (pravenel@kalrayinc.com)
 * @brief  Derived ThreadContext class for use with the Cva6CPU
 * @version 0.1
 * @date 2023-06-13
 *
 */

#pragma once

#include "config/the_isa.hh"
#include "cpu/CVA6/cpu.hh"
#include "cpu/thread_context.hh"

namespace gem5 {
namespace cva6 {

/**
 * Derived ThreadContext class for use with the Cva6CPU.
 */
class ThreadContext : public gem5::ThreadContext
{
  public:
    /** Reads this thread's PC state. */
    const PCStateBase &
    pcState() const override {
        return cpu->pcState(thread->threadId());
    }

    /** Sets this thread's PC state. */
    void pcState(const PCStateBase &val) override {
        cpu->pcState(val, thread->threadId());
    }
}

} // namespace cva6
} // namespace gem5