/**
 * minicache.hh
 *
 *  Author:   Pierre Ravenel
 * Created:   04/07/2023
 **/

#pragma once

#include <bitset>
#include <string>
#include <vector>

#include "base/named.hh"
#include "base/statistics.hh"
#include "cpu/base.hh"
#include "cpu/cva6/buffers.hh"

namespace gem5 {
namespace cva6 {

const uint64_t MC_CLSIZE = 16;

struct mc_inst_data_t
{
  bool locktag = false;
  bool hit = false;
  uint64_t value = 0;

  // _metadata
  uint8_t _is_salloc = false;
  uint8_t _is_wbs = false;
  uint8_t _need_corr = false;

  uint8_t _miss_tag = false;

  void reset(){ *this = {}; }

  bool is_taken(){ // set to 0 for debug mode
    return hit && 1;
  }
};

typedef struct mc_uint64bw_t
{

  uint8_t line[MC_CLSIZE]; // Word data
  uint64_t bw;    // Mask > size/8

  uint16_t _bw_wbs; /* bw Write by store */

  uint64_t getoffset(uint64_t addr){
    return addr & (MC_CLSIZE - 1);
  }

  uint64_t getmaskbw(uint64_t addr, uint64_t vsize){
    return ((1<< (vsize))-1) << getoffset(addr);
  }

  // Only write data which are not in bw
  void write_x(uint64_t addr, uint8_t vsize, uint8_t *data, bool isstore){
    // Write value in x
    uint64_t shift = getoffset(addr);
    for (int i = 0; i < vsize; i++){
      if (((bw >> (shift + i)) & 1) == 0){ // X bw
        assert((i + shift) < MC_CLSIZE);
        line[shift + i] = data[i];
      }
    }

    // Update mask
    uint64_t mask_bw = getmaskbw(addr, vsize);
    bw    |= mask_bw;
    if (isstore){
      _bw_wbs |= mask_bw;
    } else {
      _bw_wbs &= ~mask_bw;
    }
  }

  void write(uint64_t addr, uint8_t vsize, uint8_t *data, bool isstore){
    // Write value
    uint64_t shift = getoffset(addr);
    memcpy(line + shift, data, vsize);

    // Update mask
    uint64_t mask_bw = getmaskbw(addr, vsize);
    bw    |= mask_bw;
    if (isstore){
      _bw_wbs |= mask_bw;
    } else {
      _bw_wbs &= ~mask_bw;
    }
  }

  /** Read masked bytes:
   * A byte is returned if it is in the bw and in the user bw
  */
  uint64_t read_masked(uint64_t addr, uint8_t vsize, uint64_t mask_bw_user){
    uint64_t shift = getoffset(addr);
    uint64_t res = 0;
    // Copy data
    for (int i = 0; i < vsize; i++){
      if (((mask_bw_user & bw) >> (shift + i)) & 1){ // copy
        assert((i + shift) < MC_CLSIZE);
        res |= ((uint64_t)line[i + shift]) << (i * 8);
        // printf("res = %lx\n", res);
      }
    }
    return res;
  }

  uint64_t read(uint64_t addr, uint8_t vsize, uint64_t mask_bw_user,
    uint64_t &v)
  {
    uint64_t mask_bw = getmaskbw(addr, vsize);
    v |= read_masked(addr, vsize, mask_bw_user);
    return ~bw & mask_bw; // Return missing bw
  }

} mc_value_bw_t;

typedef struct mc_entry
{
    uint64_t addr; // Word addr
    uint16_t bw_commit; // Mask > size/8
    uint16_t bw_speculative; // Mask (speculative)

    mc_uint64bw_t val_specul;
    mc_uint64bw_t val_commit;

    bool _is_salloc;

    uint64_t lock = 0;

    bool read(uint64_t addr, uint8_t vsize, mc_inst_data_t &mcdata){
      // uint16_t shift = val_specul.getoffset(addr);
      uint16_t mask_bw = val_specul.getmaskbw(addr, vsize);

      // 0) Default value is 0
      mcdata.value = 0;
      uint16_t mask_bw_miss = mask_bw; // Read speculative bytes
      // 1) read speculative part
      mask_bw_miss = val_specul.read(addr, vsize, mask_bw_miss, mcdata.value);
      mcdata._is_wbs = (val_specul._bw_wbs & mask_bw) != 0;
      // printf("specul = %lx\n", mcdata.value);
      // 2) read commit part if needed
      if (mask_bw_miss){
        mcdata._need_corr = true;
        mcdata._is_wbs = mcdata._is_wbs ||
                         ((val_commit._bw_wbs & mask_bw) != 0);
        mask_bw_miss = val_commit.read(addr, vsize,
          mask_bw_miss, mcdata.value);
        // printf("commit = %lx\n", mcdata.value);
      }
      mcdata.hit = mask_bw_miss == 0;
      // Metadata
      mcdata._is_salloc = _is_salloc;
      // Return true (hit) if all requested bytes are present
      return mcdata.hit;
    }

    std::string dump(){
      std::stringstream ss;
      ss << std::hex;
      ss << "@" << addr << ":";
      ss << "S[" << array2str(val_specul.line, MC_CLSIZE)
         << ":" << std::bitset<MC_CLSIZE>(val_specul.bw)
         << "]";
      ss << "C[" << array2str(val_commit.line, MC_CLSIZE)
         << ":" << std::bitset<MC_CLSIZE>(val_commit.bw)
         << "]"
         << "(" << lock << ")";

      return ss.str();
    }

    void write_speculative(uint64_t addr, uint8_t vsize, uint8_t *data) {
      val_specul.write(addr, vsize, data, true);
    }
    void write_x_speculative(uint64_t addr, uint8_t vsize, uint8_t *data) {
      val_specul.write_x(addr, vsize, data, false);
    }

    void commit(uint64_t addr, uint8_t vsize, uint8_t *data, bool isstore) {
      val_commit.write(addr, vsize, data, isstore);
      if (!isstore){
        _is_salloc = false; // Reset salloc
      }
    }

    void reallocate(uint64_t key, bool isstore){
      _is_salloc = isstore;
      val_specul.bw = 0;
      //val_specul.value = 0;
      val_commit.bw = 0;
      //val_commit.value = 0;

      addr = key;
    }

    void flush(){
      // Remove the speculative part in the commit bw
      // if no commit storage uncomment:
      // entries[i].bw_commit &= ~entries[i].bw_speculative

      // And remove the speculative bw
      val_specul.bw = 0;
    }

    /** Clear both speculative and commit part */
    void clear(){
      val_specul.bw = 0;
      val_commit.bw = 0;
    }

} mc_entry_t;



/**
 *
 *
 *  FSM STATES FOR BYTES (Tag, Spec, Commit)
 *
 *  -*-*-*-*-*-*- LOAD PATH -*-*-*-*-*-*-
 *
 *  issue load -> complete load -> commit load
 *
 *     issue load : load_issue()
 *       (!@,X,X) -> ( @,0,0) : Allocate
 *       ( @,X,X) -> ( @,X,X) : -
 *  complete load : writeback_load_speculative
 *       (!@,X,X) -> (!@,X,X) : -
 *       ( @,1,X) -> ( @,1,X) : -
 *       ( @,0,X) -> ( @,1,X) : Update spec byte
 *    commit load : commit
 *       (!@,X,X) -> (!@,X,X) : No allocate
 *       ( @,X,X) -> ( @,X,1) : Set commit val
 *
 *  -*-*-*-*-*-*- STORE PATH -*-*-*-*-*-*-
 *
 *  issue store -> complete store -> commit store
 *
 *    issue store : write_speculative()
 *       (!@,X,X) -> ( @,1,0) : Allocate & set spec val
 *       ( @,X,X) -> ( @,1,X) : Set spec val
 * complete store : no events
 *       ( X,X,X) -> ( X,X,X) : -
 *   commit store : commit
 *       (!@,X,X) -> (!@,X,X) : No allocate
 *       ( @,X,X) -> ( @,X,1) : Set commit val
 *
 *  -*-*-*-*-*-*- FLUSH PATH -*-*-*-*-*-*-
 *
 *          flush : flush()
 *       ( X,X,X) -> ( X,0,X) : flush speculative BW
 *
 * FUNCS
 *
 *  lookup : (@,S,C), @  -> hit/miss
 *                      |-> hit = S | C
 *                      |-> miss = !hit
 *
 *
 */
class Minicache : public Named
{
  protected:

    struct MininicacheStats : public statistics::Group
    {
      /** Stats */
      statistics::Scalar hit;
      statistics::Scalar miss;
      statistics::Formula hitrate;

      statistics::Scalar hit_wbs;
      statistics::Scalar hit_corr;
      statistics::Scalar hit_salloc;

      statistics::Scalar miss_tag;

      MininicacheStats(BaseCPU &cpu) :
        statistics::Group(&cpu, "minicache"),
        ADD_STAT(hit, statistics::units::Count::get(),
                 "Number of hit"),
        ADD_STAT(miss, statistics::units::Count::get(),
                 "Number of miss"),
        ADD_STAT(hitrate, statistics::units::Rate<
          statistics::units::Count, statistics::units::Count>::get(),
                 "hit / (hit + miss)"),
        ADD_STAT(hit_wbs, statistics::units::Count::get(),
                 "Number of hit ^ write by store"),
        ADD_STAT(hit_corr, statistics::units::Count::get(),
                 "Number of hit ^ commit value used"),
        ADD_STAT(hit_salloc, statistics::units::Count::get(),
                 "Number of hit ^ entry was store allocated"),
        ADD_STAT(miss_tag, statistics::units::Count::get(),
                 "Number of miss when tag is abscent")
      {
        hitrate.precision(6);
        hitrate = hit / (hit + miss);
      }
    } stats;

    uint16_t size;
    mc_entry_t *entries;
    uint64_t clsize;

  public:
    Minicache(const std::string &name, BaseCPU &cpu, size_t _size)
        : Named(name), stats(cpu), size(_size),
        clsize(MC_CLSIZE) {
        entries = new mc_entry_t[size];
    }

  protected:
    /* Util function returning cache line index. -1 if None */
    int16_t get_index(uint64_t addr);

    // Policy dependant
    int16_t get_evict(uint64_t addr);
    void update_policy();
    mc_entry_t* allocate(uint64_t addr, int16_t *index_, bool isstore);

  public:
    /* Performs a lookup and return value */
    bool lookup(uint64_t addr, uint8_t vsize, mc_inst_data_t &mcdata);

    /* Performs a write and update the cache and the policy counters */
    void write_speculative(uint64_t addr, uint8_t vsize, uint64_t value,
      mc_inst_data_t &mcdata);

    /** Issue a load. It allocate an entry */
    void load_issue(uint64_t addr, mc_inst_data_t &mcdata);
    /** Complete the wb */
    void writeback_load_speculative(uint64_t addr, uint8_t vsize,
      uint8_t *data, mc_inst_data_t &mcdata);

    /* Commit the previous memory reference */
    void commit(uint64_t addr, uint8_t vsize, uint8_t *data, bool isstore,
      mc_inst_data_t &mcdata);

    void flush();
    void clear(uint64_t addr);
    void clear_all(){
      for (int i = 0; i < size; i++){
        entries[i].clear();
      }
    }
    bool isEnable() { return size != 0; }
};

} // namespace cva6
} // namespace gem5

