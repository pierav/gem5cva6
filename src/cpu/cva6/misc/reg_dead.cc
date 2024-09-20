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
          // DPRINTF(Cva6LambdaRDA, "REG_DEAD: %x : %s\n", addr, reg);
        } else {
          fatal("No register valid in : %s\n", symname);
        }
        /* Insert register in reg dead map */
        rdmap[addr].set(reg);
    }
    elf_end(elf);
    close(fd);
}

}
}
