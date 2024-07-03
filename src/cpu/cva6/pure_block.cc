#include "cpu/cva6/pure_block.hh"

#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>

#include "arch/riscv/utility.hh"
#include "cpu/cva6/registerfile.hh"
#include "debug/Cva6Commit.hh"
#include "debug/Cva6LambdaCommit.hh"
#include "debug/Cva6LambdaDump.hh"
#include "debug/Cva6LambdaLearn.hh"
#include "debug/Cva6LambdaRDA.hh"

namespace gem5 {
namespace cva6 {

inststate_t::inststate_t(Cva6DynInstPtr inst){
  fatal_if(inst->numSrcRegs() > 3, "Too mush src\n");
  fatal_if(inst->numDstRegs() > 1, "Too mush dst\n");

  int k = 0;

  // First arg is PC
  regs[k++] = inst->pc->instAddr();

  for (unsigned int i = 0; i < inst->numSrcRegs(); i++) {
    RegId reg = inst->staticInst->srcRegIdx(i);
    if (reg.classValue() != InvalidRegClass){
      regs[k++] = inst->reg_src_val[i];
    }
  }
  for (unsigned int i = 0; i < inst->numDstRegs(); i++) {
    RegId reg = inst->staticInst->destRegIdx(i);
    if (reg.classValue() != InvalidRegClass){
      regs[k++] = inst->reg_dst_val[i];
    }
  }
}

void
RegDeadAnayser::init_rdmap(const char *elfpath){
    Elf         *elf;
    Elf_Scn     *scn = NULL;
    GElf_Shdr   shdr;
    Elf_Data    *data;
    int         fd, ii, count;

    // check that header matches library version
    if (elf_version(EV_CURRENT) == EV_NONE)
        panic("wrong elf version number!");

    fd = open(elfpath, O_RDONLY);
    elf = elf_begin(fd, ELF_C_READ, NULL);

    while ((scn = elf_nextscn(elf, scn)) != NULL) {
        gelf_getshdr(scn, &shdr);
        if (shdr.sh_type == SHT_SYMTAB) {
            /* found a symbol table, go print it. */
            break;
        }
    }
    data = elf_getdata(scn, NULL);
    count = shdr.sh_size / shdr.sh_entsize;

    /* print the symbol names */
    for (ii = 0; ii < count; ++ii) {
        GElf_Sym sym;
        gelf_getsym(data, ii, &sym);
        uint64_t addr = sym.st_value;
        char *symname = elf_strptr(elf, shdr.sh_link, sym.st_name);
        /* Is a custom reg_dead symbol */
        const char *REG_DEAD_KEY = "REG_DEAD.";
        char *base = strstr(symname, REG_DEAD_KEY);
        if (base == NULL) {
          continue;
        }
        base += strlen(REG_DEAD_KEY);
        RegId reg = reverseRegisterName(cpu, base);
        DPRINTF(Cva6LambdaRDA, "REG_DEAD: %x : %s\n", addr, reg);
        /* Insert register in reg dead map */
        rdmap[addr].set(reg);
    }
    elf_end(elf);
    close(fd);
}






void
PureBlock::pushLambda(){

  while (window.size() > 1){
    DPRINTF(Cva6LambdaLearn, "*** Try to learn on:\n");
    for (int i = 0; i < window.size(); i++){
      DPRINTF(Cva6LambdaLearn, "*** WIN[%d] = %s\n", i, *window[i]);
    }

    // Fow now test if lambda is 2 -> 1
    if (rfsrc.popcount() > 2){
      break;
    }
    if (rfdst.popcount() > 1){
      break;
    }




    struct lambda_t lambda = { 0 };
    // PC
    lambda.pc = window.front()->pc->instAddr();
    lambda.size = window.size();

    // Src
    lambda.id_src_mask = rfsrc.get();
    int k = 0;
    for (int i = 0; i < 64; i++){
      if (rfsrc.isSetRaw(i)){
        lambda.src[k++] = register_src_val.getraw(i);
      }
    }

    // Dest
    lambda.id_dst_mask = rfdst.get();
    k = 0;
    for (int i = 0; i < 64; i++){
      if (rfdst.isSetRaw(i)){
        lambda.dst[k++] = register_dst_val.getraw(i);
      }
    }

    // Append Lambda
    if (lambdaBtb.count(lambda) > 0){
     lambdaBtb[lambda] += 1; // Increment
     stats.replayL += lambda.size;
     stats.Lsize.sample(lambda.size);
    } else {
      lambdaBtb[lambda] = 0; // setup
    }


    // std::ostringstream os;

    // // Lambda address
    // os << "@" << pctrigger
    //   << '<' << pccnt << '>';

    // os << std::hex;

    // // Lambda input
    // os << "(#" << register_src.count(true);
    // for (int i = 0; i < register_src.numRegs; i++){
    //   if (register_src.getraw(i)){
    //     os << ' ' << i << ':' << register_src_val.getraw(i);
    //   }
    // }

    // os << " |-> ";

    // // Lambda output
    // os << "#" << register_dst.count(true);
    // for (int i = 0; i < register_dst.numRegs; i++){
    //   if (register_dst.getraw(i)){
    //     os << ' ' << i << ':' << register_dst_val.getraw(i);
    //   }
    // }

    DPRINTF(Cva6LambdaLearn, "push Lambda [%d]: %s\n",
            lambdaBtb[lambda], lambda.str());
    // for (auto const& x : lambdaBtb)
    // {
    //   DPRINTF(Cva6Commit, "%s: %d\n", x.first.str(), x.second);
    // }
    break;
  }
    // Flush state
  register_src.setAll(false);
  register_dst.setAll(false);
  rfdst.clearall();
  rfsrc.clearall();
  window.clear();
}

void
PureBlock::commit(Cva6DynInstPtr inst) {
  uint64_t pc = inst->pc->instAddr();
  if (inst->isFault()){
    pushLambda();
    state = Idle;
    return;
  }

  // Compute instruction HASH
  inststate_t inststate(inst);
  if (infiniteBtb.count(inststate) > 0){
    infiniteBtb[inststate] += 1; // Increment
    stats.replayI += 1;
  } else {
    infiniteBtb[inststate] = 1; // setup
  }
  stats.commit += 1;


  // const int MAX_SRC = 3;
  // const int MAX_DST = 1;

  switch(state){
    case Idle:
      pctrigger = inst->pc->instAddr();
      pccnt = 0;
    [[fallthrough]];
    case Append:{
      if (inst->isFault() ||
         inst->staticInst->isStore() ||
         inst->staticInst->isControl()
        ) {
        // if (register_dst.count(true) == 1 ){
          pushLambda();
        // }
        state = Idle;
        break;
      }
      state = Append;

      // Append all regs dependancies
      for (unsigned int i = 0; i < inst->staticInst->numSrcRegs(); i++) {
        RegId reg = inst->staticInst->srcRegIdx(i);
        if (reg.classValue() != InvalidRegClass){
          if (!rfdst.isSet(reg)){ /* Is register lambda input */
            rfsrc.set(reg);
            register_src_val.set(reg, inst->getSrcRegOperand(i));
          }
          /* Perform reg dead check */
          if (rda.isRegDead(inst->pc->instAddr(), reg)){
            rfdst.clear(reg);
          }
        }
      //   if (reg.classValue() != InvalidRegClass){
      //     // Is not a previous dest register
      //     if (!register_dst.get(reg)){
      //       // Constraint number of dests
      //       if (register_src.count(true) == MAX_SRC &&
      // !register_src.get(reg)){
      //         pushLambda();
      //         state = Idle;
      //         break;
      //       }
      //       register_src.set(reg, true);
      //       register_src_val.set(reg, inst->reg_src_val[i]);
      //     }
      //   }
      }
      for (unsigned int i = 0; i < inst->staticInst->numDestRegs(); i++) {
        RegId reg = inst->staticInst->destRegIdx(i);
        if (reg.classValue() != InvalidRegClass){
          rfdst.set(reg);
          register_dst_val.set(reg, inst->getDstRegOperand(i));
        }
        // if (reg.classValue() != InvalidRegClass){
        //   // Constraint number of dests
        //   if (register_dst.count(true) == MAX_DST &&
        // !register_dst.get(reg)){
        //     pushLambda();
        //     state = Idle;
        //     break;
        //   }
        //   register_dst.set(reg, true);
        //   register_dst_val.set(reg, inst->reg_dst_val[i]);
        // }
      }
      // Increment cycle
      pccnt++;
      window.push_back(inst);

      //
      break;
    }
    case WaitEnd:{
      // Compile job :)
      // Remove regs dependancies
      // for (unsigned int i = 0; i < inst->staticInst->numDestRegs(); i++) {
      //   RegId reg = inst->staticInst->destRegIdx(i);
      //   if (reg.classValue() != InvalidRegClass){
      //     register_dst.set(reg, false);
      //   }
      // }

      fatal("TODO\n");
      break;
    }
  }

  /* Final display */
  std::stringstream ss;
  ss << *inst << ' ';
  ss << rfsrc.dump() << "->" << rfdst.dump();
  for (unsigned int i = 0; i < inst->staticInst->numSrcRegs(); i++) {
    RegId reg = inst->staticInst->srcRegIdx(i);
    ss << riscvRegisterName(reg) << ':';
    if (rda.isRegDead(pc, reg)){
      ss << "X";
    } else {
      ss << '.';
    }
    ss << ' ';
  }
  DPRINTF(Cva6LambdaCommit, "Commit %s\n", ss.str());
}

void
PureBlock::dump(){
  uint64_t total_inst = 0;
  uint64_t total_block = 0;

  for (auto const& x : lambdaBtb)
  {
    const lambda_t &lambda = x.first;
    uint64_t nb_replay = x.second;

    total_inst += nb_replay * lambda.size;
    total_block += nb_replay;
    DPRINTF(Cva6LambdaDump, "[%6ld] : %s\n",
      nb_replay, lambda.str());
  }
  // DPRINTF(Cva6LambdaDump, "#I = %ld, #LI = %ld, Block = %ld, MEAN = %f\n",
  //   stats.commit, total_inst,
  //   total_block, (float)total_inst/total_block);
}

}
}
