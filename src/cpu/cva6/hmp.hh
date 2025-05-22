/**
 * hmp.hh
 *
 *  Author:   Pierre Ravenel
 * Created:   13/02/2025
 **/


#pragma once

#include <string>
#include <vector>

#include "base/named.hh"
#include "base/statistics.hh"
#include "base/time.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "mem/packet.hh"

namespace gem5 {
namespace cva6 {

class HMP
{
  const size_t size = 1024;
  bool lht[1024] = { false };
  int64_t lhtlat[1024] = { 0 };

  // PhysicalRegFile<uint8_t>regsrchit;

  struct Stats : public statistics::Group
  {
    statistics::Scalar req;
    statistics::Scalar phah;
    statistics::Scalar pham;
    statistics::Scalar pmah;
    statistics::Scalar pmam;
    Stats(const std::string &name_, Cva6CPU &cpu) :
      statistics::Group(&cpu, name_.c_str()),
      ADD_STAT(req, "Number of requests"),
      ADD_STAT(phah, "Predicted Hit - Actual Hit"),
      ADD_STAT(pham, "Predicted Hit - Actual Miss"),
      ADD_STAT(pmah, "Predicted Miss - Actual Hit"),
      ADD_STAT(pmam, "Predicted Miss - Actual Miss") { }
  } stats;

  public:
  HMP(const std::string &name_, Cva6CPU &cpu,
    const BaseCva6CPUParams &params) : stats(name_, cpu) {}

  static bool ishit(Cva6DynInstPtr inst){
    assert(inst->dreq);
    return inst->dreq->state.inMemoryDelay() < 8;
  }

  bool predict(Cva6DynInstPtr inst){
    uint64_t pc = inst->pc->instAddr();
    // bool isregsechit = regsrchit[inst->regs_src_phy[0]];
    bool predhit = lht[(pc >> 1)%size];
    inst->vp_data.hmp_proba = lhtlat[(pc >> 1)%size];
    /*
     * @miss + Predict Miss : Hit Car HIT after miss ! (Reduct MISS)
     * @hit  + Predict Miss : Miss
     * @miss + Predict Hit  : Hit
     * @hit  + Predictw Hit  : Hit
     */
    /* How to increase miss (False hit) ? */
    //!(!predhit && isregsechit);
    return predhit;
  }

  void commit(Cva6DynInstPtr inst, bool prediction){
    uint64_t pc = inst->pc->instAddr();
    bool isahit = ishit(inst);
    /* Update predictor */
    lht[(pc >> 1)%size] = isahit;
    uint64_t old_conf = lhtlat[(pc >> 1)%size];
    if (isahit && (old_conf > 0)){
      lhtlat[(pc >> 1)%size] += 1;
    } else if (!isahit && (old_conf < 20)){
      lhtlat[(pc >> 1)%size] -= 1;
    }
    // regsrchit[inst->regs_src_phy[0]] = isahit;
    /* Some stats */
    if (prediction && isahit){ stats.phah += 1; }
    else if (prediction && !isahit) { stats.pham += 1; }
    else if (!prediction && isahit) { stats.pmah += 1; }
    else { stats.pmam += 1; }
    stats.req += 1;
  }
};
} // namespace cva6
} // namespace gem5

