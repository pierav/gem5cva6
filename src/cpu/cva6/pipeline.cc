/**
 * @file pipeline.cc
 * @author Pierre Ravenel (pravenel@kalrayinc.com)
 * @brief The full pipeline
 * @version 0.1
 * @date 2023-05-25
 *
 */

#include "cpu/cva6/pipeline.hh"
#include <chrono>
#include "debug/Cva6X.hh"

#define FAST_SIM 1
uint64_t tictac(){
    if (FAST_SIM){
        return 0;
    }
    using namespace std::chrono;
    static auto start = high_resolution_clock::now();
    auto now = high_resolution_clock::now();
    uint64_t res = duration_cast<microseconds>(now - start).count();
    start = now;
    return res;
}

namespace gem5 {
namespace cva6 {

void
Pipeline::evaluate(){
    DPRINTF(Cva6X, "-------------- PIPELINE EVALUATE ---------------\n");
    DPRINTF(Cva6X, "[F1]->[F2] %s\n", f1ToF2);
    DPRINTF(Cva6X, "[F2]->[DE] %s\n", f2ToD);
    DPRINTF(Cva6X, "[DE]->[IS] %s\n", dToIssue);
    DPRINTF(Cva6X, "[IS]->[EX] %s\n", IssueToE);
    DPRINTF(Cva6X, "[..]<-[EX] %s\n", resolved_branch);

    /** We tick the CPU to update the BaseCPU cycle counters */
    cpu.tick();

    /* Note that it's important to evaluate the stages in reverse
       order to avoid instant propagation */

    stats.systemhus += tictac(); /* Non-pipelne elapsed time */
    execute.evaluate();
    stats.exhus += tictac();
    issue.evaluate();
    stats.ishus += tictac();
    decode.evaluate();
    stats.dehus += tictac();
    fetch2.evaluate();
    stats.f2hus += tictac();
    fetch1.evaluate();
    stats.f1hus += tictac();

    /* We simulate all cycles */
    this->start();

}

} // namespace cva6
} // namespace gem5
