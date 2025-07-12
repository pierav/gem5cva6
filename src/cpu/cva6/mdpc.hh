/**
 * @file mdpc.hh
 * @author Pierre Ravenel (pravenel@kalray.eu)
 * @brief Memory Dependancy Prediction Checker
 * @version 0.1
 * @date 2025-07-08
 *
 */

#include "cpu/cva6/cpu.hh"
#include "debug/Cva6MDP.hh"

namespace gem5 {
namespace cva6 {

inline uint64_t foldn(uint64_t v, uint8_t n){
  uint64_t res = 0;
  while (v){
      res ^= v;
      v >>= n;
  }
  return res % (1 << n);
}

#define M 0b111111111111

/**
 * A simple memory order checker
 * > All stores must be issued in program order !
 **/
class MemOrderChecker : public Named
{
  struct Table : public Named
  {
    using data_t = std::pair<uint64_t /*id*/, uint64_t /*pc*/>;

    data_t inst2dat(Cva6DynInstPtr &inst){
      return {inst->id.fetchSeqNum, inst->pc->instAddr()};
    }

    std::map<uint64_t /*Key/Addr*/, data_t> lsidt; /* LastStoreID T*/

    virtual uint64_t key(Cva6DynInstPtr &inst) = 0;

    Table(std::string name) : Named(name) {}

    void markStore(Cva6DynInstPtr &inst){
      lsidt[key(inst)] = inst2dat(inst);
      DPRINTF(Cva6MDP, "MDPCW [%lx:%ld] <- %d (pc=%lx)\n",
        inst->dreq->getDWPaddr(), key(inst),
        inst->id.fetchSeqNum, inst->pc->instAddr());
    }

    uint64_t checkLoad(Cva6DynInstPtr &inst){
      DPRINTF(Cva6MDP, "MDPCR [%lx:%ld] -> %d (pc=%lx)\n",
        inst->dreq->getDWPaddr(), key(inst),
        lsidt[key(inst)].first, inst->pc->instAddr());
      return lsidt[key(inst)].first;
    }

    data_t operator[](Cva6DynInstPtr &inst){
      return lsidt[key(inst)];
    }

    Table& operator=(const Table& o){
      // lsidt = o.lsidt; TOO LONG !
      return *this;
    }

  };

  class TableH : public Table
  {
    public:
    TableH(std::string name) : Table(name) {}
    uint64_t key(Cva6DynInstPtr &inst) override {
      uint64_t addr = inst->dreq->getDWPaddr();
      return foldn(addr >> 12, 7);
    }
  };

  class TableL : public Table
  {
    public:
    TableL(std::string name) : Table(name) {}
    uint64_t key(Cva6DynInstPtr &inst) override {
      uint64_t addr = inst->dreq->getDWPaddr();
      return foldn(addr & M, 7);
    }
  };

  public: /* TODO private */
  Cva6CPU &cpu;
  // Table issue_table;
  TableH cth;
  TableL ctl;

  public:
  struct Stats : public statistics::Group
  {
    statistics::Scalar req;
    statistics::Scalar miss;

    Stats(Cva6CPU &cpu_) :
      statistics::Group(&cpu_, "mdpc"),
      ADD_STAT(req, ""),
      ADD_STAT(miss, "")
    { }
  } stats;

  MemOrderChecker(const std::string &name,
                  Cva6CPU &cpu_) : Named(name),
                  cpu(cpu_),
                  // issue_table("TI"),
                  cth(name + ".cth"),
                  ctl(name + ".ctl"),
                  stats(cpu_) {}

  void issue(Cva6DynInstPtr &inst){
    return; // Nothing to do
    // if (!inst->isFault() && inst->staticInst->isMemRef()){
    //   inst->last_store_id = issue_table.checkLoad(inst);
    //   inst->last_store_pc = issue_table[inst].second;
    // }
    // if (!inst->isFault() && inst->staticInst->isStore()){
    //   issue_table.markStore(inst);
    // }
  }

  /**
   * Return true when a memory hazard append
   */
  bool commit(Cva6DynInstPtr &inst){
    if (!inst->isFault() && inst->staticInst->isStore()){
      cth.markStore(inst);
      ctl.markStore(inst);
      // assert(commit_table[inst].first <= issue_table[inst].first);
    }
    // if (!inst->isFault() && inst->staticInst->isLoad()){
    //   return inst->last_store_id != commit_table.checkLoad(inst);
    // }
    return 0;
  }

  bool isViolation(Cva6DynInstPtr &inst, uint64_t &pcstore){
    if (inst->isFault() || !inst->staticInst->isMemRef()){
      return false;
    }
    bool is_violation = false;
    if (inst->staticInst->isLoad()){
      // uint64_t refid = commit_table[inst].first;
      // pcstore = commit_table[inst].second;
      uint64_t idh = cth[inst].first;
      uint64_t idl = ctl[inst].first;
                      // 5 < 35 && 5 < 25
      is_violation = inst->last_store_id < idh &&
                     inst->last_store_id < idl;
      /* Use != for perfect serialisation => No skip InFLight stores */
      DPRINTF(Cva6MDP, "Violation [%d] : %d < (%d, %d) \n", is_violation,
        inst->last_store_id, idh, idl);
      if (is_violation){
        if (idh < idl){
          pcstore = cth[inst].second;
        } else {
          pcstore = ctl[inst].second;
        }
      }
    } else {
      is_violation = inst->id.fetchSeqNum < inst->last_store_id;
      pcstore = inst->last_store_pc;
      if (is_violation){
        // DPRINTF(Cva6MDP, "Violation Store Store : %d != %d\n",
        //   inst->id.fetchSeqNum, inst->last_store_id);
        // FIX spec table
        assert(0);
        // issue_table.markStore(inst);
        // commit_table.markStore(inst);
      }
    }
    stats.req += 1;
    stats.miss += is_violation;
    return is_violation;
  }

  void flush(){
    /* Is there something to flush ? */
    /* TODO: the speculative table must be corrupted ! copy on flush ?*/
    // issue_table = commit_table;
  }
};


} // namespace cva6
} // namespace gem5
