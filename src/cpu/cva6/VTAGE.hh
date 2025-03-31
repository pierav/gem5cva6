/*
 * Authors: NathanaÃ«l PrÃ©millieu
 */

#pragma once

#include <cstdlib>
#include <deque>
#include <vector>

#include "base/named.hh"
#include "base/statistics.hh"
#include "base/types.hh"

// #include "cpu/o3/ConfCounter.hh"

#define VTAGE_CONF_MAX 15

namespace gem5 {
namespace cva6 {
typedef uint64_t Prediction;

// Forward definitions
class ghist_t;
class hist_entry_t;

/** this is the cyclic shift register for folding
 * a long global history into a smaller number of bits;
 * see P. Michaud's PPM-like predictor at CBP-1
 */
class Folded_history
{
  public:
    uint64_t comp;
    unsigned clength;
    unsigned olength;
    unsigned outpoint;
    Folded_history() {}
    void init(int original_length, int compressed_length) ;
    void update(std::deque<hist_entry_t> &globHist) ;
};

struct FoldedHistories
{
  size_t size=5;
  /** Utility for computing TAGE indices */
  Folded_history i[5];
  /** Utility for computing TAGE tags */
  Folded_history t[2][5];
  uint64_t path=0;

  FoldedHistories() {};
  FoldedHistories(
    uint64_t size_,
    std::vector<unsigned> logg,
    /** Log of number of entries  on each tagged component */
    std::vector<unsigned> m
    /** Used for storing the history lengths */
  );
  void updatefull(ghist_t &ghist);
  void dump();
};

typedef std::pair<uint64_t, uint64_t> prediction_t;

/** We still have to save the cyclic register when a branch is encountered
 * because the branch predictor does not necessarily use registers of the same
 * size. We also have to save the index and the tag of the entry, as well as
 * the bank and the altbank and which one made the prediction. This is only
 * needed for speculative execution.
 */
struct VPSave
{
  // Indexes and tags used to access the components.
  std::vector<unsigned> gI;
  std::vector<unsigned> gTag;

  // Selected bank and altBank
  unsigned bank;
  unsigned altBank;

  // In VTAGE, altPred is only used to update the u counter, it is never used
  // as the prediction.
  prediction_t tagePred;
  prediction_t altPred;

  // Branch instructions create histories as they update the internal folded
  // history registers.
  bool isBranch;
  bool usedAlt;
};

struct ConfCounter
{
  int64_t conf = 0;
  ConfCounter() {}
  uint64_t read() { return conf; }
  bool saturated() { return conf == VTAGE_CONF_MAX; }
  void updateConf(bool valid) {
    if (valid && conf < VTAGE_CONF_MAX){
      conf += 1;
    }
    if (!valid && conf > 0){
      conf = 0;
    }
  }
  void set(uint64_t val) {
    conf = val;
  }
};

class VTageVP : public Named
{
public:
  /**
   * Default branch predictor constructor.
   */
  VTageVP(const std::string &name, size_t size);

  FoldedHistories initialch(){
    return FoldedHistories(numHistComponents + 1, logg, m);
  }

  /**
   * Registers statistics. Gem5 specific.
   */
  void regstatistics();


  void setch(FoldedHistories &ch_);

  /**
   * Looks up the given address in the branch predictor and returns
   * a pair <value, confidence>.  Also creates a
   * BPHistory object to store any state it will need on squash/update.
   * @param[branch_addr] The address of instruction to look up.
   * @param[micropc] The uop index.
   * @param[vp_history Pointer that will point to the VPSave object.
   * @return The predicted result of the instruction.
   */
  prediction_t lookup(uint64_t &branch_addr, VPSave *vp_history);

  /**
   * Updates the value predictor with the actual result of an instruction.
   * @param[val] Actual result.
   * @param[vp_history] Pointer to the VPSave object that was created
   * when the value was predicted.
   * @param[mispred] The prediction is incorrect.
   * @param[squashed] is set when this function is called during a squash
   * operation.
   */
  void update(Prediction &val, VPSave *vp_history, bool mispred,
    bool squashed);

  /**
   * Restores the global value history on a squash.
   * @param[vp_history] Pointer to the VPHistory object that has the
   * previous cyclic registers in it.
   * @param[remove] True if the VPSave corresponding to the instruction should
   * be deleted, false otherwise.
   * @param[recompute] True if the cyclic registers should be recomputed
   * (squashing a branch).
   */
  void squash(VPSave *vp_history, bool remove, bool recompute);

  /** Computes the new folded history when a branch is encountered.
   * @param[save] True if the cyclic registers should be saved.
   * @param[vp_history] Pointer to the VPSave object that has the
   * previous cyclic registers in it.
   * @pre The buffer managing the branch history has been updated by the branch
   *predictor.
   * @post Folded histories are ready to compute new indexes.
   **/
  void updateFoldedHist(bool save, VPSave *vp_history, uint64_t seqnum = 0);

  /** Update the branch predictor with the committed value.
   * @param[val] The actual result of the instruction.
   * @param[outcome] True if the predicition was correct, false otherwise.
   * @param[vp_history] Pointer to the VPSave object that has the
   * previous cyclic registers in it.
   * @param[squashed] True if the instruction caused a squashed
   * (cannot be true if the predictor is not updated on squashes).
   */
  void updatePredictor(Prediction &val, bool outcome, VPSave *vp_history,
                       bool squashed);

  std::string dump();


private:
  /** Index function for the base table */
  inline unsigned bIndex(uint64_t &branch_addr) {
    return ((branch_addr >> instShiftAmt) & baseMask);
  }

  /**
   * The index functions for the tagged tables uses
   * path history as in the OGEHL predictor
   */
  /** F serves to mix path history */
  unsigned F(unsigned A, unsigned size, unsigned bank);

  /** gIndex computes a full hash of pc, ghist and path hist */
  unsigned gIndex(uint64_t &branch_addr, unsigned bank);

  /** tags computation */
  unsigned gTag(uint64_t &branch_addr, unsigned bank);

  /** Base prediction (with the base predictor) */
  prediction_t getBasePred(uint64_t &branch_addr, bool &saturated);


  /**
   * Just a simple pseudo random number generator:
   * a 2-bit counter, used to avoid ping-pong phenomenon
   * on tagged entry allocations
   */
  inline unsigned myRandom() {
    ++seed;
    return seed & 3;
  }


  void updatestatistics(VPSave &history, bool outcome);

  /** VTAGE base table entry */
  class Bentry
  {
  public:
    ConfCounter hyst;
    Prediction pred;
    Bentry() {}

    /**
     * Updates the hysteresis/confidence counter
     * @param[outcome] True if the prediction was correct, false otherwise.
     * @param[val] The actual result of the instruction.
     **/
    void ctrupdate(bool outcome, Prediction &val) {
      if (hyst.read() == 0) {
        pred = val;
      }
      hyst.updateConf(outcome);
    }
  };

  /** VTAGE global table entry */
  class Gentry
  {
  public:
    ConfCounter hyst;
    unsigned tag = 0;
    unsigned u = 0;
    Prediction pred;
    Gentry() {}

    /**
     * Updates the hysteresis/confidence counter
     * @param[outcome] True if the prediction was correct, false otherwise.
     * @param[val] The actual result of the instruction.
     **/
    void ctrupdate(bool outcome, Prediction &val) {
      if (hyst.read() == 0) {
        pred = val;
      }
      hyst.updateConf(outcome);
    }
  };

  /** Parameters */
  /** Number of Tagged Components */
  unsigned numHistComponents;

  /** "Use alternate prediction on newly allocated":
   * a 4-bit counter  to determine whether the newly
   * allocated entries should be considered as
   * valid or not for delivering  the prediction */
  int useAltOnNA;

  /** Control counter for the smooth resetting of useful counters */
  unsigned logCTick, cTick;

  /** Log of number of entries  on each tagged component */
  std::vector<unsigned> logg;

  /** Utility for computing TAGE indices */
  FoldedHistories ch;

  /** Base VTAGE table */
  std::vector<Bentry> bTable;

  /** Tagged VTAGE tables */
  std::vector<std::vector<Gentry>> gTable;

  /** Used for storing the history lengths */
  std::vector<unsigned> m;

  /** For the pseudo-random number generator */
  unsigned seed;

  /** Mask to compute the index in the base bimodal predictor */
  uint64_t baseMask;

  /** Mask to compute the tag for the different tagged tables */
  std::vector<uint64_t> tagMask;

  /** Mask to compute the index in the different tagged tables */
  std::vector<uint64_t> gMask;
  std::vector<unsigned> proba;
  /**
   * Count how many prediction have been made
   * since the last misprediction due to the base predictor
   */
  unsigned sinceBaseMispred;

  /** Number of bits to shift the instruction over to get rid of the word
   *  offset.
   */
  unsigned instShiftAmt;


  /** Global statistics. Gem5 specific */
  statistics::Scalar hit;
  statistics::Scalar req;


  // statistics for frontend predictions
  statistics::Scalar taggedHit;
  statistics::Scalar altTaggedHit;
  statistics::Distribution table;

  // statistics for backend predictions
  /** Stat for number of predictions given by the tagged components. */
  statistics::Scalar taggedPred;
  /** Stat for number of predictions given by the base bimodal predictor. */
  statistics::Scalar basePred;
  /** Stat for number of standard predictions. */
  statistics::Scalar standardPred;
  /** Stat for number of predictions given by the alternate prediction. */
  statistics::Scalar altPred;

  statistics::Scalar alloc;

  statistics::Scalar attempted;
  statistics::Scalar correct;
  statistics::Scalar incorrect;

  statistics::Formula accuracy;
  statistics::Formula coverage;
  statistics::Formula nonTagged;
  statistics::Scalar correctBasePred;
  statistics::Scalar commit_correct, commit_incorrect;
  statistics::Formula accuracy_ideal, coverage_ideal;

  /** Confidence statistics */
  /** Stat for number of high confidence predictions. */
  statistics::Scalar highConf;
  /** Stat for number of correct high confidence predictions. */
  statistics::Scalar highConfHit;
  /** Stat for the number of incorrect high confidence predictions */
  statistics::Scalar highConfMiss;
  /** Stat for number of medium confidence predictions. */
  statistics::Scalar transientConf;
  /** Stat for number of correct medium confidence predictions. */
  statistics::Scalar transientConfHit;
  /** Stat for the number of incorrect medium confidence predictions */
  statistics::Scalar transientConfMiss;
  /** Stat for number of low confidence predictions. */
  statistics::Scalar lowConf;
  /** Stat for number of correct low confidence predictions. */
  statistics::Scalar lowConfHit;
  /** Stat for the number of incorrect medium confidence predictions */
  statistics::Scalar lowConfMiss;

  /** Stat for number of high confidence base predictions. */
  statistics::Scalar baseHighConf;
  /** Stat for number of correct high confidence base bimodal predictions. */
  statistics::Scalar baseHighConfHit;
  /** Stat for the number of incorrect high confidence base bimodal predictions
   */
  statistics::Scalar baseHighConfMiss;
  /** Stat for number of medium confidence base bimodal predictions. */
  statistics::Scalar baseTransientConf;
  /** Stat for number of correct medium confidence base bimodal predictions. */
  statistics::Scalar baseTransientConfHit;
  /** Stat for the number of incorrect medium confidence base bimodal
   * predictions */
  statistics::Scalar baseTransientConfMiss;
  /** Stat for number of low confidence base bimodal predictions. */
  statistics::Scalar baseLowConf;
  /** Stat for number of correct low confidence base bimodal predictions. */
  statistics::Scalar baseLowConfHit;
  /** Stat for the number of incorrect medium confidence base bimodal
   * predictions */
  statistics::Scalar baseLowConfMiss;

  statistics::Distribution accesses;
};

} // namespace cva6
} // namespace gem5
