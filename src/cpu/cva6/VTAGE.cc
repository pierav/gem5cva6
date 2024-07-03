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


VTageVP::VTageVP(const std::string &name, unsigned _numHistComponents,
                 unsigned numLogBaseEntry, unsigned minHistSize,
                 unsigned maxHistSize, unsigned _baseHystShift,
                 unsigned counterWidth, unsigned _instShiftAmt,
                 std::vector<unsigned> &_filterProbability, ghist_t *ghist_)
    : Named(name), numHistComponents(_numHistComponents),
      baseHystShift(0), useAltOnNA(0), logCTick(19),
      cTick((1 << (logCTick - 1))), seed(0), instShiftAmt(1), ghist(ghist_) {
  proba = _filterProbability;

  // For sanity checking.
  created = deleted = 0;

  unsigned i;
  unsigned size = numHistComponents + 1;

  /** logg */
  numLogBaseEntry = 9;
  assert(size == 5);
  logg.resize(size);
  logg[4] = 8; // 256
  logg[3] = 7; // 128
  logg[2] = 6; // 64
  logg[1] = 6; // 64

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
    std::cerr << m[i] << std::endl;
  }

  /** ch_i, ch_t */
  ch_i.resize(size);
  ch_t[0].resize(size);
  ch_t[1].resize(size);

  for (i = 1; i < size; ++i) {
    ch_i[i].init(m[i], logg[i]);
    ch_t[0][i].init(ch_i[i].olength, 12 + i);
    ch_t[1][i].init(ch_i[i].olength, 12 + i - 1);
  }

  /** bTable */
  bTable.resize(1 << numLogBaseEntry, Bentry(counterWidth, proba));

  /** gTable */
  gTable.resize(size);
  for (i = 1; i < size; ++i) {
    gTable[i].resize(1 << logg[i], Gentry(counterWidth, proba));
  }

  /** Compute masks */
  baseMask = ((1 << (numLogBaseEntry)) - 1);

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
          ch_i[bank].comp ^ F(ghist->getPath(), M, bank);
  return (unsigned)(index & gMask[bank]);
}

unsigned VTageVP::gTag(uint64_t &branch_addr, unsigned bank) {
  uint64_t tag = branch_addr ^ ch_t[0][bank].comp ^ (ch_t[1][bank].comp << 1);
  return (unsigned)(tag & tagMask[bank]);
}

prediction_t VTageVP::getBasePred(uint64_t &branch_addr, bool &saturated) {
  unsigned index = bIndex(branch_addr);

  saturated = bTable[index].hyst.saturated();
  return prediction_t(bTable[index].pred, bTable[index].hyst.read());
}

void VTageVP::baseUpdate(uint64_t branch_addr, Prediction &val, bool outcome,
                         unsigned conf, bool squashed) {
  unsigned index = bIndex(branch_addr);
  bTable[index].ctrupdate(outcome, val, conf);
}

prediction_t VTageVP::lookup(uint64_t &addr, void *&vp_history,
                             uint64_t seqNum) {

  int i;
  unsigned size = numHistComponents + 1;
  unsigned hitBank = 0;
  unsigned altBank = 0;
  prediction_t tagePred;
  prediction_t altPred;

  bool choseAlt;
  bool baseSaturated = false;

  // Create VTageVP::VPSave. This is the history for a new instruction.
  VPSave *history = new VPSave();
  ++created;

  // TAGE prediction
  // computes the table addresses and the partial tags
  history->gI.resize(size);
  history->gTag.resize(size);

  uint64_t taddr = addr;

  history->gI[0] = bIndex(taddr);

  for (i = 1; i < size; ++i) {
    history->gI[i] = gIndex(taddr, i);
    history->gTag[i] = gTag(taddr, i);
  }

  for (i = size - 1; i > 0; --i) {
    bool hit = gTable[i][history->gI[i]].tag == history->gTag[i];
    DPRINTF(ValuePredictor, "[%c] BANK[%x]:"
        "index: %d, tag: %d==%d?, pred %lx\n",
        hit ? 'X' : ' ', i, history->gI[i],
        gTable[i][history->gI[i]].tag, history->gTag[i],
         gTable[i][history->gI[i]].pred);
  }
  DPRINTF(ValuePredictor, "[.] BANK[0]: index: %d, pred %lx\n",
    history->gI[0], bTable[history->gI[0]].pred);

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

  vp_history = static_cast<void *>(history);
  hists.insert(std::pair<void *, uint64_t>(vp_history, seqNum));
  return (choseAlt ? altPred : tagePred);
}

void VTageVP::updateFoldedHist(bool save, void *&vp_history, uint64_t seqnum) {
  unsigned size = numHistComponents + 1;
  unsigned i = 0;
  DPRINTF(ValuePredictor, "Updating Folded History in VTAGE\n");

  if (save) {
    VPSave *new_record = new VPSave();
    ++created;

    new_record->ch_i_comp.resize(size);
    new_record->ch_t_comp[0].resize(size);
    new_record->ch_t_comp[1].resize(size);

    for (i = 1; i < size; ++i) {
      // DPRINTF(ValuePredictor, "Saving folded history %u: %lu\n", i,
      // ch_i[i].comp);

      new_record->ch_i_comp[i] = ch_i[i].comp;
      new_record->ch_t_comp[0][i] = ch_t[0][i].comp;
      new_record->ch_t_comp[1][i] = ch_t[1][i].comp;
    }

    new_record->bank = 0;
    new_record->altBank = 0;
    new_record->tagePred = prediction_t(Prediction(), 0);
    new_record->altPred = prediction_t(Prediction(), 0);
    new_record->isBranch = true;

    vp_history = static_cast<void *>(new_record);
  }

  // prepare next index and tag computations

  for (i = 1; i < size; ++i) {
    ch_i[i].update(ghist->getRaw());
    // DPRINTF(ValuePredictor, "VTAGE: Updated folded history %u: %lu\n", i,
    // ch_i[i].comp);
    ch_t[0][i].update(ghist->getRaw());
    ch_t[1][i].update(ghist->getRaw());
    DPRINTF(
        ValuePredictor,
        "Updating ch_i[%u] to %u, ch_t[0][%u] to %u and ch_t[1][%u] to %u\n",
        i, ch_i[i].comp, i, ch_t[0][i].comp, i, ch_t[1][i].comp);
  }
  hists.insert(std::pair<void *, uint64_t>(vp_history, seqnum));
  DPRINTF(ValuePredictor, "ValuePred: GlobHist size: %i, new path hist: %x\n",
          ghist->getRaw().size(), ghist->getPath());
}

void VTageVP::updatePredictor(Prediction &val, bool outcome, void *vp_history,
                              bool squashed) {

  unsigned i, j;
  unsigned size = numHistComponents + 1;
  unsigned nrand = myRandom();
  prediction_t tagePred;
  prediction_t altPred;

  unsigned hitBank;
  unsigned altBank;

  assert(vp_history);

  VPSave *history = static_cast<VPSave *>(vp_history);
  table.sample(history->bank);
  // Propagate the confidence instead of re reading the table.
  unsigned conf = history->tagePred.second;

  if (squashed) {
    if (history->bank == 0) {
      baseUpdate(history->gI[0], val, false, 0, true);
    } else {
      gTable[history->bank][history->gI[history->bank]].ctrupdate(false, val,
                                                                  0);
    }
    return;
  }

  tagePred = history->tagePred;
  altPred = history->altPred;
  hitBank = history->bank;
  altBank = history->altBank;

#ifndef NOREAD_AT_COMMIT_VTAGE
  if (hitBank == 0) {
    outcome = bTable[history->gI[0]].pred == val && outcome;
  } else {
    outcome = gTable[hitBank][history->gI[hitBank]].pred == val && outcome;
  }
#endif

  // VTAGE UPDATE
  // try to allocate a  new entries only if prediction was wrong
  bool alloc = !outcome && (hitBank < numHistComponents);
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
    gTable[hitBank][history->gI[hitBank]].ctrupdate(outcome, val, conf);

    // if the provider entry is not certified to be useful also update the
    // alternate prediction
    if (gTable[hitBank][history->gI[hitBank]].u == 0) {
      if (altBank > 0) {
        accesses.sample(altBank);
        gTable[altBank][history->gI[altBank]].ctrupdate(outcome, val,
                                                        altPred.second);
      }
      if (altBank == 0) {
        // baseUpdate(addr, val, altPred.first == val);
        accesses.sample(altBank);
        baseUpdate(history->gI[0], val, outcome, altPred.second);
      }
    }
  } else {
    accesses.sample(0);
    // baseUpdate(addr, val, altPred.first == val);
    baseUpdate(history->gI[0], val, outcome, conf);
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
  updatestatistics(*history, outcome);
}

void VTageVP::squash(void *vp_history, bool remove, bool recompute) {
  VPSave *hist = static_cast<VPSave *>(vp_history);
  if (!remove && !recompute) {
    assert(!hist->isBranch);
  }
  if (hist->isBranch) {
    DPRINTF(ValuePredictor, "squashing a branch in value predictor\n");
    recoverBHist(vp_history, recompute);
  }
  if (remove)
    recoverVHist(vp_history);
}

void VTageVP::flush_branch(void *vp_history) {
  VPSave *history = static_cast<VPSave *>(vp_history);
  // DPRINTF(ValuePredictor, "Deleting 0x%lx\n", history);
  hists.erase(vp_history);
  delete history;
  ++deleted;
  assert(created - deleted < 512);
}

void VTageVP::recoverBHist(/*uint64_t &addr,*/ void *vp_history,
                           bool recompute) {
  unsigned size = numHistComponents + 1;
  unsigned i;

  VPSave *rollback = static_cast<VPSave *>(vp_history);

  /** Restore history */
  assert(vp_history);
  assert(rollback->isBranch);

  for (i = 1; i < size; ++i) {
    ch_i[i].comp = rollback->ch_i_comp[i];
    ch_t[0][i].comp = rollback->ch_t_comp[0][i];
    ch_t[1][i].comp = rollback->ch_t_comp[1][i];

    DPRINTF(
        ValuePredictor,
        "Restoring ch_i[%u] to %u, ch_t[0][%u] to %u and ch_t[1][%u] to %u\n",
        i, ch_i[i].comp, i, ch_t[0][i].comp, i, ch_t[1][i].comp);
  }

  // Call to recompute the cyclic registers with the correct global branch
  // history Only if needed.
  if (recompute)
    updateFoldedHist(false, vp_history);
}

void VTageVP::recoverVHist(/*uint64_t &addr,*/ void *vp_history) {
  // Essentially we don't have much to do here.
  VPSave *history = static_cast<VPSave *>(vp_history);
  // DPRINTF(ValuePredictor, "Deleting 0x%lx\n", history);
  hists.erase(vp_history);
  delete history;
  deleted++;
  assert(created - deleted < 512);
}

void VTageVP::update(Prediction &val, void *vp_history, bool mispred,
                     bool squashed) {
  hit += !mispred;
  req += 1;
  VPSave *history = static_cast<VPSave *>(vp_history);
  assert(!history->isBranch);

  DPRINTF(ValuePredictor, "Updating the predictor with %x"
          " Predicted %x\n", val,
          history->tagePred.first);

  updatePredictor(
      /*addr,*/ val,
      !mispred && /*(history->usedAlt ? (val == history->altPred.first) :*/ (
                      val == history->tagePred.first),
      vp_history, squashed);
  // DPRINTF(ValuePredictor, "Deleting 0x%lx\n", history);
  if (!squashed) {
#ifdef DEBUG
    assert(hists.find(vp_history) != hists.end());
    uint64_t seqNum = hists[vp_history];
    for (auto it = hists.begin(); it != hists.end(); ++it) {
      if (it->second < seqNum) {
        std::cerr << "Should not happen, inst " << std::dec << it->second
                  << "it still here, is branch : "
                  << static_cast<VPSave *>(it->first)->isBranch << std::endl;
        fatal("oups");
      }
    }
#endif
    delete history;
    hists.erase(vp_history);
    deleted++;
    assert(created - deleted < 512);
  }
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
