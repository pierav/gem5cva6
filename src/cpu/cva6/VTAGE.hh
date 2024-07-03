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

struct ConfCounter
{
  ConfCounter() {}
  ConfCounter(uint64_t _width, uint64_t XX, std::vector<unsigned> *_proba) {}
  uint64_t read() { return 1; }
  bool saturated() { return false; }
  void updateConf(bool outcome) { /**/
  }
  void set(uint64_t val) {}
};

typedef std::pair<uint64_t, uint64_t> prediction_t;

class VTageVP : public Named
{
public:
  /**
   * Default branch predictor constructor.
   * @param[name] Name of the class (will appear in the stats). Gem5 Specific.
   * @param[numHistComponents] Number of tagged tables.
   * @param[numLogBaseEntry] Log2(#entries) of the base component.
   * @param[minHistSize] Minimum bhist length.
   * @param[maxHistSize] Max bhist length.
   * @param[baseHystShift] In case hysteresis counters have to be shared btw
   * entries. NOT IN VTAGE.
   * @param[counterWidth] Width of the confidence counters.
   * @param[instShiftAmt] Shifts the PC (useful only for RISC or aligned
   * instructions).
   * @param[filterProbability] The probability vector to handle forward
   * transitions of confidence counters.
   * @param[tage] Pointer to TAGE (to debug internal state of the folded
   * registers).
   *
   * @pre assert(filterProbability.size()) == exp(2, counterWidth) - 1;
   */
  VTageVP(const std::string &name, unsigned numHistComponents,
          unsigned numLogBaseEntry, unsigned minHistSize, unsigned maxHistSize,
          unsigned baseHystShift, unsigned counterWidth, unsigned instShiftAmt,
          std::vector<unsigned> &filterProbability, ghist_t *ghist);


/** We still have to save the cyclic register when a branch is encountered
 * because the branch predictor does not necessarily use registers of the same
 * size. We also have to save the index and the tag of the entry, as well as
 * the bank and the altbank and which one made the prediction. This is only
 * needed for speculative execution.
 */
  struct VPSave
  {
    // Copy of the folded registers to be able to restore them on a squash.
    std::vector<uint64_t> ch_i_comp;
    std::vector<uint64_t> ch_t_comp[2];

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

    uint64_t instuint64_t;

    bool usedAlt;
  };

  /**
   * Registers statistics. Gem5 specific.
   */
  void regstatistics();

  /**
   * Looks up the given address in the branch predictor and returns
   * a pair <value, confidence>.  Also creates a
   * BPHistory object to store any state it will need on squash/update.
   * @param[branch_addr] The address of instruction to look up.
   * @param[micropc] The uop index.
   * @param[vp_history Pointer that will point to the VPSave object.
   * @param[seqNum] The sequence number of the instruction causing the lookup.
   * @return The predicted result of the instruction.
   */
  prediction_t lookup(uint64_t &branch_addr, void *&vp_history,
                      uint64_t seqNum = 0);

  /**
   * Updates the value predictor with the actual result of an instruction.
   * @param[val] Actual result.
   * @param[vp_history] Pointer to the VPSave object that was created
   * when the value was predicted.
   * @param[mispred] The prediction is incorrect.
   * @param[squashed] is set when this function is called during a squash
   * operation.
   */
  void update(Prediction &val, void *vp_history, bool mispred, bool squashed);

  /**
   * Restores the global value history on a squash.
   * @param[vp_history] Pointer to the VPHistory object that has the
   * previous cyclic registers in it.
   * @param[remove] True if the VPSave corresponding to the instruction should
   * be deleted, false otherwise.
   * @param[recompute] True if the cyclic registers should be recomputed
   * (squashing a branch).
   */
  void squash(void *vp_history, bool remove, bool recompute);

  /** Computes the new folded history when a branch is encountered.
   * @param[save] True if the cyclic registers should be saved.
   * @param[vp_history] Pointer to the VPSave object that has the
   * previous cyclic registers in it.
   * @pre The buffer managing the branch history has been updated by the branch
   *predictor.
   * @post Folded histories are ready to compute new indexes.
   **/
  void updateFoldedHist(bool save, void *&vp_history, uint64_t seqnum = 0);

  /** Update the branch predictor with the committed value.
   * @param[val] The actual result of the instruction.
   * @param[outcome] True if the predicition was correct, false otherwise.
   * @param[vp_history] Pointer to the VPSave object that has the
   * previous cyclic registers in it.
   * @param[squashed] True if the instruction caused a squashed
   * (cannot be true if the predictor is not updated on squashes).
   */
  void updatePredictor(Prediction &val, bool outcome, void *vp_history,
                       bool squashed);

  std::string dump();

  // Basically delete the saved folded history for a branch instruction when
  // they are no longer necessary (because we are committing/updating the
  // predictor with a younger instruction).
  void flush_branch(void *vp_history);

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
   * Update the base predictor.
   */
  void baseUpdate(uint64_t branch_addr, Prediction &val, bool taken,
                  unsigned conf, bool squashed = false);

  /**
   * Just a simple pseudo random number generator:
   * a 2-bit counter, used to avoid ping-pong phenomenon
   * on tagged entry allocations
   */
  inline unsigned myRandom() {
    ++seed;
    return seed & 3;
  }

  /**
   * Recover from a misprediction: correct the global branch history.
   * @param[vp_history] Pointer to the VPSave object coressponding to the
   * faulting instruction.
   */
  void recoverBHist(void *vp_history, bool recompute);
  void recoverVHist(void *vp_history);

  void updatestatistics(VPSave &history, bool outcome);

  /** VTAGE base table entry */
  class Bentry
  {
  public:
    ConfCounter hyst;
    Prediction pred;

    /**
     * @param[counterwidth] Width of the confidence counter in bits.
     * @param[proba] Vector containing the 2^counterbits probabilities
     * controlling the forward transitions of the counter.
     */
    Bentry(unsigned counterwidth, std::vector<unsigned> &proba) {
      pred = Prediction();
      hyst = ConfCounter(counterwidth, 0, &proba);
    }

    /**
     * Updates the hysteresis/confidence counter
     * @param[outcome] True if the prediction was correct, false otherwise.
     * @param[val] The actual result of the instruction.
     * @param[conf] The confidence read at fetch.
     * @param[squashed] True is update on squash, false otherwise.
     **/
    void ctrupdate(bool outcome, Prediction &val, unsigned conf) {
#ifdef NOREAD_AT_COMMIT_VTAGE
      hyst.set(conf);
#endif

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
    unsigned tag;
    unsigned u;
    Prediction pred;

    /**
     * @param[counterwidth] Width of the confidence counter in bits.
     * @param[proba] Vector containing the 2^counterbits probabilities
     * controlling the forward transitions of the counter.
     */
    Gentry(unsigned counterwidth, std::vector<unsigned> &proba) {
      hyst = ConfCounter(counterwidth, 0, &proba);
      pred = Prediction();
      tag = 0;
      u = 0;
    }

    /**
     * Updates the hysteresis/confidence counter
     * @param[outcome] True if the prediction was correct, false otherwise.
     * @param[val] The actual result of the instruction.
     * @param[conf] The confidence read at fetch.
     * @param[squashed] True is update on squash, false otherwise.
     **/
    void ctrupdate(bool outcome, Prediction &val, unsigned conf) {
#ifdef NOREAD_AT_COMMIT_VTAGE
      hyst.set(conf);
#endif
      if (hyst.read() == 0) {
        pred = val;
      }

      hyst.updateConf(outcome);
    }
  };

  /** Parameters */
  /** Number of Tagged Components */
  unsigned numHistComponents;

  /** sharing an hysteresis bit between 4 bimodal predictor entries */
  unsigned baseHystShift;

  /** Internal structures and variables */

  uint64_t created, deleted;

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
  std::vector<Folded_history> ch_i;

  /** Utility for computing TAGE tags */
  std::vector<Folded_history> ch_t[2];

  /** Base VTAGE table */
  std::vector<Bentry> bTable;

  /** Tagged VTAGE tables */
  std::vector<std::vector<Gentry>> gTable;

  /** Used for storing the history lengths */
  std::vector<unsigned> m;

  /** Map to store VPSave objects ie track all the inflight state of each
   * prediction */
  std::map<void *, uint64_t> hists;

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

  /* Pointer to the global history */
  ghist_t *ghist;

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
