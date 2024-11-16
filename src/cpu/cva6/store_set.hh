/**
 * store_set.hh
 *
 *  Author:   Pierre Ravenel
 * Created:   15/11/2024
 **/


#pragma once

#include <string>
#include <vector>

#include "base/statistics.hh"
#include "cpu/cva6/dyn_inst.hh"

namespace gem5 {
namespace cva6 {

template<class T>
struct valid_value_t
{
  T value;
  bool valid = false;
  void set(T& val){
    value = val;
    valid = true;
  }
};

template<class T>
class StoreSet
{
  private:
  using SSID = uint64_t;
  using ssit_t = valid_value_t<SSID>;
  using lfst_t = valid_value_t<T>;
  /** The Store Set ID Table. */
  std::vector<ssit_t> SSIT;
  /** Last Fetched Store Table. */
  std::vector<lfst_t> LFST;

  uint64_t num_req = 0;

  /** Calculates the index into the SSIT based on the pc. */
  inline ssit_t& ssi(Addr pc) { return SSIT[(pc >> 2) % SSIT.size()]; }
  /** Calculates a Store Set ID based on the pc. */
  inline SSID calcSSID(Addr pc) { return (pc ^ (pc >> 10)) % LFST.size(); }

  public:
  StoreSet(size_t n) {
    SSIT.resize(n);
    LFST.resize(n);
  }

  /** Records a memory ordering violation between the younger load
   * and the older store. */
  void violation(Addr store_pc, Addr load_pc){
    ssit_t& s_ssi = ssi(store_pc);
    ssit_t& l_ssi = ssi(load_pc);
    if (!l_ssi.valid && !s_ssi.valid){
      SSID set = calcSSID(load_pc);
      s_ssi.set(set);
      l_ssi.set(set);
    } else if (l_ssi.valid && !s_ssi.valid){
      SSID set = l_ssi.value;
      s_ssi.set(set);
    } else if (!l_ssi.valid && s_ssi.valid){
      SSID set = s_ssi.value;
      l_ssi.set(set);
    } else { /* Both valid */
      l_ssi.value = s_ssi.value = std::min(s_ssi.value, l_ssi.value);
    }
  }

  void pushStore(Addr store_pc, T id){
    checkClear();
    ssit_t& s_ssi = ssi(store_pc);
    if (!s_ssi.valid){
      return;
    }
    LFST[s_ssi.value].set(id);
  }

  void popStore(Addr store_pc, T id){
    ssit_t& s_ssi = ssi(store_pc);
    if (!s_ssi.valid){
      return;
    }
    lfst_t& s_lfs = LFST[s_ssi.value];
    if (s_lfs.valid && (s_lfs.value == id)){
      s_lfs.valid = false;
    }
  }

  bool checkInst(Addr pc, T* id){
    checkClear();
    ssit_t& l_ssi = ssi(pc);
    if (!l_ssi.valid){
      return false;
    }
    lfst_t& lfs = LFST[l_ssi.value];
    if (!lfs.valid){
      return false;
    }
    *id = lfs.value;
    return true;
  }

  void checkClear(){
    if ((num_req++ % 250000) == 0){
      clear();
    }
  }

  void clear(){
    for (auto &e: SSIT){
      e.valid = false;
    }
    for (auto &e: LFST){
      e.valid = false;
    }
  }
};

} // namespace cva6
} // namespace gem5

