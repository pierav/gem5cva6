/**
 * @file pipeline.cc
 * @author your name (pravenel@kalray.eu)
 * @brief
 * @version 0.1
 * @date 2023-05-25
 *
 */
#include "cpu/cva6/pipeline.hh"

#include "debug/Cva6X.hh"

namespace gem5 {
namespace cva6 {

void
Pipeline::evaluate(){
    DPRINTF(Cva6X, "-------------- PIPELINE EVALUATE ---------------\n");

    /* Update the time buffers before the stages */
    f1ToF2.evaluate();
    // f2ToD.evaluate();
    // dToIssue.evaluate();
    // IssueToE.evaluate();
   //  resolved_branch.evaluate();

    DPRINTF(Cva6X, "[F1]->[F2] %s\n", f1ToF2.dump().c_str());
    DPRINTF(Cva6X, "[F2]->[DE] %s\n", f2ToD.dump().c_str());
    DPRINTF(Cva6X, "[DE]->[IS] %s\n", dToIssue.dump().c_str());
    DPRINTF(Cva6X, "[IS]->[EX] %s\n", IssueToE.dump().c_str());
    DPRINTF(Cva6X, "[..]<-[EX] %s\n", resolved_branch.dump().c_str());

    /** We tick the CPU to update the BaseCPU cycle counters */
    cpu.tick();

    /* Note that it's important to evaluate the stages in reverse
       order to avoid instant propagation */
    execute.evaluate();
    issue.evaluate();
    decode.evaluate();
    fetch2.evaluate();
    fetch1.evaluate();

    /* We simulate all cycles */
    this->start();

}

} // namespace cva6
} // namespace gem5
