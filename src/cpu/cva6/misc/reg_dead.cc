/**
 * reg_dead.cc
 *
 *  Author:   Pierre Ravenel
 * Created:   03/09/2024
 **/
#include "cpu/cva6/misc/reg_dead.hh"

#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>

#include "debug/Cva6LambdaRDA.hh"

namespace gem5 {
namespace cva6 {

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
        if (base == NULL) { /* Not a reg dead symbol */
          continue;
        }
        base += strlen(REG_DEAD_KEY);
        if (strcmp(base, "arg") == 0){ /* An useless reg dead */
          warn("Not an isa register: %s\n", symname);
          continue;
        }
        RegId reg;
        if (reverseRegisterName(cpu, base, reg)){
          DPRINTF(Cva6LambdaRDA, "REG_DEAD: %x : %s\n", addr, reg);
        } else {
          fatal("No register valid in : %s\n", symname);
        }
        /* Insert register in reg dead map */
        rdmap[addr].set(reg);
    }
    elf_end(elf);
    close(fd);
}



bool
RegDeadAnayser::check_reg_dead_at_commit(Cva6DynInstPtr inst){
  /* Check reg dead */
  if (inst->isFault()){
    return false;
  }
  if (inst->staticInst->isStore()){
    stats.stores += 1;
  }
  bool ret = false;
  // Check all src regs
  for (unsigned int i = 0; i < inst->staticInst->numSrcRegs(); i++) {
    RegId reg = inst->staticInst->srcRegIdx(i);
    if (reg.classValue() != InvalidRegClass){
      if (inst->exec_data.pmode == 0){// USER
        if (!rf_used.isSet(reg)){
          Cva6DynInstPtr guilty = rf_freeer.get(reg);
          if (guilty){
            if (!inst->staticInst->isStore()){
                warn("RaRD %s from: %s to: %s\n",
                  reg, *guilty, *inst);
            } else {
                stats.stores_dead += 1;
                ret = true;
                // TODO check SP
                // warn("Store uses DEAD REG : %s\n", *inst);
            }
              //cpu.pipeline->rda.clear(guilty->pc->instAddr(), reg);
          }
        }
      }
    }
  }
  // free reg dead
  for (unsigned int i = 0; i < inst->staticInst->numSrcRegs(); i++) {
    RegId reg = inst->staticInst->srcRegIdx(i);
    if (inst->exec_data.is_reg_dead[i]){
      rf_used.clear(reg);
      rf_freeer.set(reg, inst);
    }
  }
  // Set all dest regs
  for (unsigned int i = 0; i < inst->staticInst->numDestRegs(); i++) {
    RegId reg = inst->staticInst->destRegIdx(i);
    if (reg.classValue() != InvalidRegClass){
      rf_used.set(reg);
    }
  }
  return ret;
}

}
}
