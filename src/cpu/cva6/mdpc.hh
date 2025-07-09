/**
 * @file mdpc.hh
 * @author Pierre Ravenel (pravenel@kalray.eu)
 * @brief Memory Dependancy Prediction Checker
 * @version 0.1
 * @date 2025-07-08
 *
 */

#include "cpu/cva6/cpu.hh"

namespace gem5 {
namespace cva6 {

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

    std::map<uint64_t /*Addr*/, data_t> lsidt; /* LastStoreID T*/
    Table(std::string name) : Named(name) {}

    void markStore(Cva6DynInstPtr &inst){
      assert(inst->dreq);
      lsidt[inst->dreq->getDWPaddr()] = inst2dat(inst);
      DPRINTF(Cva6MDP, "MDPCW [%lx] <- %d\n", inst->dreq->getDWPaddr(),
        inst->id.fetchSeqNum);
    }

    uint64_t checkLoad(Cva6DynInstPtr &inst){
      assert(inst->dreq);
      DPRINTF(Cva6MDP, "MDPCR [%lx] <- %d\n", inst->dreq->getDWPaddr(),
        lsidt[inst->dreq->getDWPaddr()].first);
      return lsidt[inst->dreq->getDWPaddr()].first;
    }

    data_t operator[](Cva6DynInstPtr &inst){
      return lsidt[inst->dreq->getDWPaddr()];
    }

    Table& operator=(const Table& o){
      // lsidt = o.lsidt; TOO LONG !
      return *this;
    }

  };

  public: /* TODO private */
  Cva6CPU &cpu;
  Table issue_table;
  Table commit_table;

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
                  issue_table("TI"),
                  commit_table("TC"),
                  stats(cpu_) {}

  void issue(Cva6DynInstPtr &inst){
    if (!inst->isFault() && inst->staticInst->isMemRef()){
      inst->last_store_id = issue_table.checkLoad(inst);
      inst->last_store_pc = issue_table[inst].second;
    }
    if (!inst->isFault() && inst->staticInst->isStore()){
      issue_table.markStore(inst);
    }
  }

  /**
   * Return true when a memory hazard append
   */
  bool commit(Cva6DynInstPtr &inst){
    if (!inst->isFault() && inst->staticInst->isStore()){
      commit_table.markStore(inst);
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
      uint64_t refid = commit_table[inst].first;
      pcstore = commit_table[inst].second;

      is_violation = inst->last_store_id < refid;
      /* Use != for perfect serialisation => No skip InFLight stores */
      DPRINTF(Cva6MDP, "Violation [%d] : %d != %d\n", is_violation,
        inst->last_store_id, refid);
    } else {
      is_violation = inst->id.fetchSeqNum < inst->last_store_id;
      pcstore = inst->last_store_pc;
      if (is_violation){
        DPRINTF(Cva6MDP, "Violation Store Store : %d != %d\n",
          inst->id.fetchSeqNum, inst->last_store_id);
        // FIX spec table
        assert(0);
        issue_table.markStore(inst);
        commit_table.markStore(inst);
      }
    }
    stats.req += 1;
    stats.miss += is_violation;
    return is_violation;
  }

  void flush(){
    /* Is there something to flush ? */
    /* TODO: the speculative table must be corrupted ! copy on flush ?*/
    issue_table = commit_table;
  }
};


} // namespace cva6
} // namespace gem5
