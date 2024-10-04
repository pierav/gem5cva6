#include "cpu/cva6/pure_block.hh"

#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>

#include "arch/riscv/utility.hh"
#include "cpu/cva6/lambda_types.hh"
#include "cpu/cva6/lambda_utils.hh"
#include "cpu/cva6/registerfile.hh"
#include "debug/Cva6Commit.hh"
#include "debug/Cva6LambdaCommit.hh"
#include "debug/Cva6LambdaDump.hh"
#include "debug/Cva6LambdaLearn.hh"

namespace gem5 {
namespace cva6 {


void
InstStatesHandler::commit(Cva6DynInstPtr inst){
  // Compute instruction HASH
  // inststate_t inststate(inst);
  /* Keep track of K last committed instructions */
  committed.push_front(inst);
  committed.pop_back();


  const bool enable_suffix_tree = true;
  if (enable_suffix_tree){ /* Suffix pattern match */

    /* Compute instruction unique key */
    const state_t* state = &*memsave.insert(state_t(inst)).first;

    /* Read tree */
    size_t depth = suffixtree.insert(state);
    assert(depth <= NB_INFLIGHT);

    size_t depth_const_load = 0;
    for (int i = 0; i < depth; i++){
      if (committed[i]->exec_data.is_const_load){
        depth_const_load++;
      } else {
        break;
      }
    }

    size_t depth_silent_store = 0;
    for (int i = 0; i < depth; i++){
      if (committed[i]->exec_data.is_silent_store){
        depth_silent_store++;
      } else {
        break;
      }
    }

    BinaryRegisterFile rfsrc;
    BinaryRegisterFile rfdst;
    size_t depth_lambda21_best = 0;
    size_t depth_lambdak1_best = 0;

    for (int i = 0; i < depth; i++){
      Cva6DynInstPtr inst = committed[i];
      // Append all regs dependancdies
      for (unsigned int i = 0; i < inst->staticInst->numSrcRegs(); i++) {
        RegId reg = inst->staticInst->srcRegIdx(i);
        if (reg.classValue() != InvalidRegClass){
          if (!rfdst.isSet(reg)){ /* Is register lambda input */
            rfsrc.set(reg);
          }
          /* Perform reg dead check */
          if (inst->exec_data.is_reg_dead[i]){
            rfdst.clear(reg);
          }
        }
      }
      /* Append all destination registers */
      for (unsigned int i = 0; i < inst->staticInst->numDestRegs(); i++) {
        RegId reg = inst->staticInst->destRegIdx(i);
        if (reg.classValue() != InvalidRegClass){
          rfdst.set(reg);
        }
      }
      /* Evaluate potential lambda */
      if (rfsrc.popcount() <= 2 && rfdst.popcount() <= 1){
        depth_lambda21_best = i+1;
      }
      if (rfdst.popcount() <= 1){
        depth_lambdak1_best = i+1;
      }
    }

    size_t depth_clss = std::min(depth_const_load, depth_silent_store);
    stats.req += 1;
    stats.replay[depth] += 1;
    stats.replay_cl[depth_const_load] += 1;
    stats.replay_ss[depth_silent_store] += 1;
    stats.replay_clss[depth_clss] += 1;

    stats.replay_rd[depth_lambda21_best] += 1;
    stats.replay_rdk1[depth_lambdak1_best] += 1;
    // /* Iterate over suffix */
    // // std::cout << "Path (#" << depth << "):" << std::endl;
    // for (auto &e: suffixtree){
    //   // std::cout << "e: " << *e << std::endl;
    // }
  }
}


void
PureBlock::commit(Cva6DynInstPtr inst) {
  #if 0
  if (inst->l_data.is_predicted_last){ /* Uop check inserttion */
    /* Create Static inst */
    StaticInstPtr si = new LambdaCheckInst(i2id(prediction.rd),
      prediction.rd_val);
    assert(si);
    /* Create the compound Dynamic instruction */
    i2 = new Cva6DynInst(&cpu, si, &inst->pc);
    i2->id.fetchSeqNum = 1; // Setup fake sequence number
    assert(i2->staticInst);
    // DPRINTF(Cva6LambdaLearn, "Issue %s\n", *i2);

    assert(i2);
    assert(i2->numSrcRegs() == 1);
    const RegId &reg = i2->srcRegIdx(0); /* Issue stage instruction */
    uint64_t value = cpu.thread->getReg(reg); /* Read Register */
    i2->setSrcRegOperand(0, inst->l_data.check_val);
    i2->executeComplete(); /* Execution stage */
    i2 = nullptr; /* Free instruction */
  }
  #endif
  // inst_state_handler.commit(inst);
}

}
}
