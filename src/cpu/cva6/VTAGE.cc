/*
 * Authors: NathanaÃ«l PrÃ©millieu
 */

#include "cpu/cva6/VTAGE.hh"

#include <cmath>
#include <cstdlib>
#include <sstream>

#include "base/intmath.hh"
#include "base/trace.hh"
#include "cpu/cva6/vp_dpe.hh"
#include "debug/ValuePredictor.hh"

namespace gem5 {
namespace cva6 {

void
Folded_history::init(int original_length, int compressed_length) {
    comp = 0;
    olength = original_length;
    clength = compressed_length;
    outpoint = olength % clength;
}

void
Folded_history::update(std::deque<hist_entry_t> &globHist) {
    comp = (comp << 1) | (uint64_t)globHist[0].dir;
    comp ^= ((uint64_t)globHist[olength].dir << outpoint);
    comp ^= (comp >> clength);
    comp &= (1 << clength) - 1;
}

FoldedHistories::FoldedHistories(
  uint64_t size_,
  std::vector<unsigned> logg,
  std::vector<unsigned> m
){
  size = size_;
  assert(size == 5);
  // i.resize(size);
  // t[0].resize(size);
  // t[1].resize(size);
  for (int k = 1; k < size; ++k) {
    i[k].init(m[k], logg[k]);
    t[0][k].init(m[k], 12 + k);
    t[1][k].init(m[k], 12 + k - 1);
  }
}

void
FoldedHistories::updatefull(ghist_t &ghist){
  // Fix ch
  for (int k = 0; k < size; k++){
    i[k].comp = ghist.getDirFold(i[k].olength, i[k].clength);
    t[0][k].comp = ghist.getDirFold(t[0][k].olength, t[0][k].clength);
    t[1][k].comp = ghist.getDirFold(t[1][k].olength, t[1][k].clength);
  }
  path = ghist.getPath();
  // for (int k = 1; k < size; ++k) {
  //   i[k].update(ghist->getRaw());
  //   t[0][k].update(ghist->getRaw());
  //   t[1][k].update(ghist->getRaw());
  // }
}

void
FoldedHistories::dump(){
  DPRINTF(ValuePredictor, "cd(%d):\n",size);
  for (int k = 1; k < size; ++k) {
    DPRINTF(ValuePredictor, "i[%d]=%lx t[0][%d]=%lx t[1][%d]=%lx\n",
      k, i[k].comp,
      k, t[0][k].comp,
      k, t[1][k].comp);
  }
}

VTageVP::VTageVP(const std::string &name, uint64_t size_)
    : Named(name), numHistComponents(4),
      useAltOnNA(0), logCTick(19),
      cTick((1 << (logCTick - 1))), seed(0), instShiftAmt(1) {
  unsigned i;
  unsigned size = numHistComponents + 1;
  unsigned minHistSize = 4;
  unsigned maxHistSize = 64;

  /** logg */
  uint64_t logsize = std::log2(size_);
  logg.resize(size);
  logg[0] = logsize-1; // 512
  logg[1] = logsize-2; // 256
  logg[2] = logsize-3; // 128
  logg[3] = logsize-4; // 64
  logg[4] = logsize-4; // 64

  // 512, 256, 128, 64, 64 ?
  // for (i = 1; i < size; ++i) {
  //   logg[i] = 10;
  // }

  /** m */
  m.resize(size);
  /** computes the geometric history lengths */
  m[1] = minHistSize;
  m[numHistComponents] = maxHistSize;

  for (i = 2; i < size; i++) {
    m[i] = (int)(((double)minHistSize *
                  pow((double)(maxHistSize) / (double)minHistSize,
                      (double)(i - 1) / (double)((numHistComponents - 1)))) +
                 0.5);
  }

  std::cout << "VTAGE";
  for (int i = 0; i < size; i++){
    std::cout << "-" << (1<<logg[i]) << "(H" << m[i] << ")";
  }
  std::cout << std::endl;

  ch = FoldedHistories(size, logg, m);

  /** bTable */
  bTable.resize(1 << logg[0], Bentry());

  /** gTable */
  gTable.resize(size);
  for (i = 1; i < size; ++i) {
    gTable[i].resize(1 << logg[i], Gentry());
  }

  /** Compute masks */
  baseMask = ((1 << (logg[0])) - 1);
  tagMask.resize(size);
  for (i = 1; i < size; ++i) {
    tagMask[i] = ((1 << (i + 12)) - 1);
  }
  gMask.resize(size);
  for (i = 1; i < size; ++i) {
    gMask[i] = ((1 << logg[i]) - 1);
  }
  regstatistics();
}

void VTageVP::regstatistics() {


  table.name(name() + ".TableSpread").init(0, 6, 1);
  hit.name(name() + ".hit").desc("hit");
  req.name(name() + ".req").desc("req");


  accesses.init(0, 6, 1)
      .name(name() + ".accesses")
      .desc("Number of accesses for each table");

  accuracy.name(name() + ".VPaccuracyAll")
      .desc("Accuracy of the predictor (committed predictions)")
      .precision(5);

  coverage.name(name() + ".VPcoverageAll")
      .desc("Coverage of the predictor (committed prediction)")
      .precision(5);

  taggedHit.name(name() + ".taggedHit")
      .desc("Number of hits in a tagged table during a lookup"
            " for the standard "
            "pred");

  altTaggedHit.name(name() + ".altTaggedHit")
      .desc("Number of hits in a tagged table during a lookup"
            " for the altpred");

  alloc.name(name() + ".alloc")
      .desc("Number of allocations in tagged components");

  attempted.name(name() + ".attempted")
      .desc("Total number of attempted predictions that "
            "proceeded to commit");

  correct.name(name() + ".correct")
      .desc("Total number of correct predictions that proceeded to commit");

  incorrect.name(name() + ".incorrect")
      .desc("Total number of incorrect predictions that proceeded to commit");

  accuracy_ideal.name(name() + ".accuracy_ideal").desc("Accuracy ideal");

  coverage_ideal.name(name() + ".coverage_ideal").desc("Coverage ideal");

  commit_correct.name(name() + ".commit_correct")
      .desc("Total number of correct predictions ideal");

  commit_incorrect.name(name() + ".commit_incorrect")
      .desc("Total number of incorrect predictions ideal");

  taggedPred.name(name() + ".taggedPred")
      .desc("Number of tagged components predictions");

  basePred.name(name() + ".basePred")
      .desc("Number of base bimodal predictions");

  standardPred.name(name() + ".standardPred")
      .desc("Number of standard predictions");

  altPred.name(name() + ".altPred")
      .desc("Number of predictions given by the alternate prediction");

  highConf.name(name() + ".highConf")
      .desc("Number of high confidence predictions");

  highConfHit.name(name() + ".highConfHit")
      .desc("Number of correct high confidence predictions");

  highConfMiss.name(name() + ".highConfMiss")
      .desc("Number of incorrect high confidence predictions.");

  transientConf.name(name() + ".transientConf")
      .desc("Number of transient confidence predictions");

  transientConfHit.name(name() + ".transientConfHit")
      .desc("Number of correct transient confidence predictions");

  transientConfMiss.name(name() + ".transientConfMiss")
      .desc("Number of incorrect transient confidence predictions.");

  lowConf.name(name() + ".lowConf")
      .desc("Number of low confidence predictions");

  lowConfHit.name(name() + ".lowConfHit")
      .desc("Number of correct low confidence predictions");

  lowConfMiss.name(name() + ".lowConfMiss")
      .desc("Number of incorrect low confidence predictions.");

  baseHighConf.name(name() + ".baseHighConf")
      .desc("Number of high confidence base predictions");

  baseHighConfHit.name(name() + ".baseHighConfHit")
      .desc("Number of correct high confidence base redictions");

  baseHighConfMiss.name(name() + ".baseHighConfMiss")
      .desc("Number of incorrect high confidence base predictions.");

  baseTransientConf.name(name() + ".baseTransientConf")
      .desc("Number of transient confidence base predictions");

  baseTransientConfHit.name(name() + ".baseTransientConfHit")
      .desc("Number of correct medium confidence base predictions");

  baseTransientConfMiss.name(name() + ".baseTransientConfMiss")
      .desc("Number of incorrect medium confidence base predictions.");

  baseLowConf.name(name() + ".baseLowConf")
      .desc("Number of low confidence base predictions");

  baseLowConfHit.name(name() + ".baseLowConfHit")
      .desc("Number of correct low confidence base bimodal predictions");

  baseLowConfMiss.name(name() + ".baseLowConfMiss")
      .desc("Number of incorrect low confidence base bimodal predictions.");

  nonTagged.name(name() + ".nonTagged")
      .desc(
          "Number of correct predictions that flowed from the base predictor");

  correctBasePred.name(name() + ".correctBasPred")
      .desc(
          "Number of correct predictions that flowed from the base"
          " predictors");

  coverage = correct / attempted;
  accuracy = correct / (correct + incorrect);
  nonTagged = correctBasePred / attempted;

  coverage_ideal = commit_correct / attempted;
  accuracy_ideal = commit_correct / (commit_correct + commit_incorrect);
}

unsigned VTageVP::F(unsigned hist, unsigned size, unsigned bank) {
  uint64_t res, h1, h2;
  res = (uint64_t)hist;
  res = res & ((1 << size) - 1);
  h1 = (res & gMask[bank]);
  h2 = (res >> logg[bank]);
  h2 = ((h2 << bank) & gMask[bank]) + (h2 >> (logg[bank] - bank));
  res = h1 ^ h2;
  res = ((res << bank) & gMask[bank]) + (res >> (logg[bank] - bank));
  return (unsigned)res;
}

unsigned VTageVP::gIndex(uint64_t &branch_addr, unsigned bank) {
  uint64_t index;
  unsigned M = (m[bank] > 16) ? 16 : m[bank];
  index = branch_addr ^ (branch_addr >> (abs(logg[bank] - bank) + 1)) ^
          ch.i[bank].comp ^ F(ch.path, M, bank);
  return (unsigned)(index & gMask[bank]);
}

unsigned VTageVP::gTag(uint64_t &branch_addr, unsigned bank) {
  uint64_t tag = branch_addr ^ ch.t[0][bank].comp ^ (ch.t[1][bank].comp << 1);
  return (unsigned)(tag & tagMask[bank]);
}

prediction_t VTageVP::getBasePred(uint64_t &branch_addr, bool &saturated) {
  unsigned index = bIndex(branch_addr);

  saturated = bTable[index].hyst.saturated();
  return prediction_t(bTable[index].pred, bTable[index].hyst.read());
}

prediction_t VTageVP::lookup(uint64_t &addr, VPSave *history) {

  int i;
  unsigned size = numHistComponents + 1;
  unsigned hitBank = 0;
  unsigned altBank = 0;
  prediction_t tagePred;
  prediction_t altPred;

  bool choseAlt;
  bool baseSaturated = false;

  // TAGE prediction
  // computes the table addresses and the partial tags
  history->gI.resize(size);
  history->gTag.resize(size);

  uint64_t taddr = addr;

  /* Tags and indices  */
  history->gI[0] = bIndex(taddr);
  for (i = 1; i < size; ++i) {
    history->gI[i] = gIndex(taddr, i);
    history->gTag[i] = gTag(taddr, i);
  }

  for (i = size - 1; i > 0; --i) {
    bool hit = gTable[i][history->gI[i]].tag == history->gTag[i];
    DPRINTF(ValuePredictor, "[%c] BANK[%x]:"
        "index: %d, tag: %d==%d?, pred %lx (%d/7)\n",
        hit ? 'X' : ' ', i, history->gI[i],
        gTable[i][history->gI[i]].tag, history->gTag[i],
        gTable[i][history->gI[i]].pred,
        gTable[i][history->gI[i]].hyst.read()
    );
  }
  DPRINTF(ValuePredictor, "[.] BANK[0]:index: %d, pred %lx (%d/7)\n",
    history->gI[0], bTable[history->gI[0]].pred,
    bTable[history->gI[0]].hyst.read());

  // Look for the bank with longest matching history
  for (i = size - 1; i > 0; --i) {
    if (gTable[i][history->gI[i]].tag == history->gTag[i]) {
      hitBank = i;
      break;
    }
  }

  // Look for the alternate bank
  for (i = hitBank - 1; i > 0; --i) {
    if (gTable[i][history->gI[i]].tag == history->gTag[i]){
      altBank = i;
      break;
    }
  }
  DPRINTF(ValuePredictor, "hitBank = %d, altBank = %d\n",
          hitBank, altBank);


  // computes the prediction and the alternate prediction
  if (hitBank > 0) {
    if (altBank > 0) {
      altPred = prediction_t(
          gTable[altBank][history->gI[altBank]].pred,
          (uint8_t)gTable[altBank][history->gI[altBank]].hyst.read());
    } else {
      altPred = getBasePred(addr, baseSaturated);
    }
    // if the entry is recognized as a newly allocated entry and
    // useAltOnNA is positive  use the alternate prediction
    tagePred = prediction_t(
        gTable[hitBank][history->gI[hitBank]].pred,
        (uint8_t)gTable[hitBank][history->gI[hitBank]].hyst.read());

    if ((useAltOnNA < 0) ||
        gTable[hitBank][history->gI[hitBank]].hyst.read() > 0) {
      choseAlt = false;

      DPRINTF(ValuePredictor,
              "ValuePred: 0x%lx, standard prediction used, vtagePred %lu\n",
              addr, tagePred.first);
    } else {
      choseAlt = true;
    }
  } else {
    altPred = getBasePred(addr, baseSaturated);
    tagePred = prediction_t(altPred.first, altPred.second);
    choseAlt = false;
  }
  // PR: Removed Confidence
  choseAlt = false;

  /* save predictor state */
  if (!history)
    panic("No history data structure to save the predictor state");

  /* save prediction, hit/alt bank, GI and GTAG */
  history->bank = hitBank;
  history->altBank = altBank;
  history->tagePred = tagePred;
  history->altPred = altPred;
  history->isBranch = false;
  DPRINTF(ValuePredictor, "VPSave :bank: %i, altbank: %i, choseAlt: %x\n",
          history->bank, history->altBank, choseAlt);
  return (choseAlt ? altPred : tagePred);
}


void VTageVP::updatePredictor(Prediction &val, bool outcome, VPSave *history,
                              bool squashed) {

  unsigned i, j;
  unsigned size = numHistComponents + 1;
  unsigned nrand = myRandom();
  prediction_t tagePred;
  prediction_t altPred;

  unsigned hitBank;
  unsigned altBank;

  assert(history);
  table.sample(history->bank);

  if (squashed) {
    if (history->bank == 0) {
      bTable[history->gI[0]].ctrupdate(false, val);
    } else {
      gTable[history->bank][history->gI[history->bank]].ctrupdate(false, val);
    }
    return;
  }

  tagePred = history->tagePred;
  altPred = history->altPred;
  hitBank = history->bank;
  altBank = history->altBank;

  bool valid;
  if (hitBank == 0) {
    valid = bTable[history->gI[0]].pred == val; // && outcome;
  } else {
    valid = gTable[hitBank][history->gI[hitBank]].pred == val; // && outcome;
  }

  // VTAGE UPDATE
  // try to allocate a  new entries only if prediction was wrong
  bool alloc = !valid && (hitBank < numHistComponents);
  if (hitBank > 0) {
    // Manage the selection between longest matching and alternate matching
    // for "pseudo"-newly allocated longest matching entry
    bool PseudoNewAlloc =
        (gTable[hitBank][history->gI[hitBank]].hyst.read() == 0);
    // an entry is considered as newly allocated if its prediction counter is
    // weak
    if (PseudoNewAlloc) {
      if (tagePred.first == val) {
        alloc = false;
      }
      // if it was delivering the correct prediction, no need to allocate a new
      // entry
      // even if the overall prediction was false
      if (altPred.second == 7) {
        if (!(altPred.first == tagePred.first)) {
          if (altPred.first == val) {
            if (useAltOnNA < 7) {
              useAltOnNA++;
            }
          } else if (useAltOnNA > -8) {
            useAltOnNA--;
          }
        }
      }
    }
  }

  if (alloc) {
    // is there some "unuseful" entry to allocate
    unsigned min = 1;
    for (i = numHistComponents; i > hitBank; --i) {
      accesses.sample(i);
      if (gTable[i][history->gI[i]].u < min) {
        min = gTable[i][history->gI[i]].u;
      }
    }
    // we allocate an entry with a longer history
    // to  avoid ping-pong, we do not choose systematically the next entry, but
    // among the 3 next entries
    unsigned Y = nrand & ((1 << (numHistComponents - hitBank - 1)) - 1);
    unsigned X = hitBank + 1;
    if (Y & 1) {
      X++;
      if (Y & 2)
        X++;
    }
    // NO ENTRY AVAILABLE:  ENFORCES ONE TO BE AVAILABLE
    // TODO Do we need this?

    if (min > 0) {
      gTable[X][history->gI[X]].u = 0;
    }

    // Allocate only  one entry
    for (i = X; i < size; ++i) {
      accesses.sample(i);
      if (gTable[i][history->gI[i]].u == 0) {
        this->alloc++;

        gTable[i][history->gI[i]].tag = history->gTag[i];
        gTable[i][history->gI[i]].hyst.set(0);
        gTable[i][history->gI[i]].u = 0;
        gTable[i][history->gI[i]].pred = val;

        // TODO Fix DPRINTF
        DPRINTF(ValuePredictor,
                "ValuePred: new entry allocated,"
                " table: %i, index: %i, tag: %i, value: %lx\n",
                i, history->gI[i], history->gTag[i], val);

        break;
      }
    }
  }
  // periodic reset of u
  cTick++;
  if ((cTick & ((1 << logCTick) - 1)) == 0) {
    // reset least significant bit
    // most significant bit becomes least significant bit
    for (i = 1; i < size; ++i) {
      for (j = 0; j < (1 << logg[i]); ++j) {
        gTable[i][j].u = 0;
      }
    }
  }

  if (hitBank > 0) {
    accesses.sample(hitBank);
    gTable[hitBank][history->gI[hitBank]].ctrupdate(valid, val);

    // if the provider entry is not certified to be useful also update the
    // alternate prediction
    if (gTable[hitBank][history->gI[hitBank]].u == 0) {
      if (altBank > 0) {
        accesses.sample(altBank);
        gTable[altBank][history->gI[altBank]].ctrupdate(valid, val);
      }
      if (altBank == 0) {
        accesses.sample(altBank);
        bTable[history->gI[0]].ctrupdate(valid, val);
      }
    }
  } else {
    accesses.sample(0);
    bTable[history->gI[0]].ctrupdate(valid, val);
  }

  // update the u counter
  if (!(tagePred.first == altPred.first)) {
    if (tagePred.first == val && hitBank > 0) {
      gTable[hitBank][history->gI[hitBank]].u = 1;
    } else if (hitBank > 0) {
      gTable[hitBank][history->gI[hitBank]].u = 0;
    }
  } else if (altPred.second != 7 && tagePred.first == val && hitBank > 0) {
    gTable[hitBank][history->gI[hitBank]].u = 1;
  }
  updatestatistics(*history, valid);
}

void VTageVP::setch(FoldedHistories &ch_) {
  ch = ch_;
}

void VTageVP::update(Prediction &val, VPSave *history, bool valid,
                     bool squashed) {
  hit += valid;
  req += 1;
  DPRINTF(ValuePredictor, "Updating the predictor with %x"
          " Predicted %x\n", val,
          history->tagePred.first);

  updatePredictor(val, valid, history, squashed);
}

void VTageVP::updatestatistics(VPSave &history, bool outcome) {
  /** Base stats **/

  bool proceeded = /*(history.usedAlt && history.altPred.second == 7) ||
                      (!history.usedAlt &&*/
      (history.tagePred.second == 7);
  attempted++;

  correct += outcome && proceeded;
  incorrect += !outcome && proceeded;

  /*if (history.usedAlt)
  {
          altPred++;
          if (history.altBank == 0)
          {
                  basePred++;
                  if (outcome && proceeded) {
                          correctBasePred++;
                  }
          } else {
                  taggedPred++;
          }
  } else {*/
  standardPred++;
  if (history.bank == 0) {
    basePred++;
    if (outcome && proceeded) {
      correctBasePred++;
    }
  } else {
    taggedPred++;
  }
  //}

  /** statistics for the tagged components **/

  bool highConf = (/*!history.usedAlt &&*/ history.bank != 0 &&
                   history.tagePred.second >= 7);

  bool transient = /*((!history.usedAlt &&*/ (history.bank != 0 &&
                                              history.tagePred.second >= 1);

  bool low_conf = /*((!history.usedAlt &&*/ (history.bank != 0 &&
                                             history.tagePred.second == 0);
  // || (history.usedAlt && history.altBank != 0 && history.altPred.second ==
  // 0)) && !highConf && !transient;

  this->highConf += highConf;
  highConfHit += highConf && outcome;
  highConfMiss += highConf && !outcome;

  transientConf += transient;
  transientConfHit += transient && outcome;
  transientConfMiss += transient && !outcome;

  lowConf += low_conf;
  lowConfHit += low_conf && outcome;
  lowConfMiss += low_conf && !outcome;

  // statistics for the base component.

  highConf =
      (!history.usedAlt && history.bank == 0 &&
         history.tagePred.second >= 7) ||
      (history.usedAlt && history.altBank == 0 &&
        history.altPred.second >= 7);

  transient = ((!history.usedAlt && history.bank == 0 &&
                history.tagePred.second >= 1) ||
               (history.usedAlt && history.altBank == 0 &&
                history.altPred.second >= 1)) &&
              !highConf;

  low_conf = ((!history.usedAlt && history.bank == 0 &&
               history.tagePred.second == 0) ||
              (history.usedAlt && history.altBank == 0 &&
               history.altPred.second == 0)) &&
             !highConf && !transient;

  baseHighConf += highConf;
  baseHighConfHit += highConf && outcome;
  baseHighConfMiss += highConf && !outcome;

  baseTransientConf += transient;
  baseTransientConfHit += transient && outcome;
  baseTransientConfMiss += transient && !outcome;

  baseLowConf += low_conf;
  baseLowConfHit += low_conf && outcome;
  baseLowConfMiss += low_conf && !outcome;
}

std::string VTageVP::dump() {
  std::ostringstream oss;
  oss << "VPred: Branch history: TODO";
  oss << std::endl;
  return oss.str();
}

} // namespace cva6
} // namespace gem5
