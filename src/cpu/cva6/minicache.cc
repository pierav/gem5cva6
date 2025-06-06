/**
 * minicache.cc
 *
 *  Author:   Pierre Ravenel
 * Created:   04/07/2023
 **/

#include "cpu/cva6/minicache.hh"

#include <iomanip>
#include <iostream>
#include <queue>
#include <sstream>
#include <string>

#include "base/named.hh"
#include "debug/Cva6Minicache.hh"

#define MINICACHE_ADDR_F(addr, clsize) ((addr) >> log2i(clsize))

namespace gem5 {
namespace cva6 {


std::string array2str(uint8_t *data, uint64_t size){
  std::stringstream ss;
  ss << std::hex << std::setfill('0');
  for (int i = size-1; i >= 0; i--) {
      ss << std::hex << std::setw(2) << static_cast<int>(data[i]);
  }
  return ss.str();
}

int16_t
Minicache::get_index(uint64_t addr){
    addr = MINICACHE_ADDR_F(addr, clsize);
    int16_t index = addr % size; // Direct MAP
    return addr == entries[index].addr ? index : -1;
    // for (int i = 0; i < size; i++){
    //     if (addr == entries[i].addr){
    //         return i;
    //     }
    // }
    // return -1;
}

int16_t
Minicache::get_evict(uint64_t addr){
    return MINICACHE_ADDR_F(addr, clsize) % size; // Direct MAP
    // return rand() % size;
}

void
Minicache::update_policy(){
    /* Nothing to do */
}

bool
Minicache::lookup(uint64_t addr, uint8_t vsize, mc_inst_data_t &mcdata){
    int16_t index = get_index(addr);
    if (index == -1){ // Miss Tag
        mcdata._miss_tag = true;
        DPRINTF(Cva6Minicache, "lookup miss @%x, size%x\n", addr, vsize);
        return false;
    }
    mc_entry_t *entry = &entries[index];
    entry->read(addr, vsize, mcdata);
    DPRINTF(Cva6Minicache, "lookup %s @%08x, size=%1x, line[%2d] = %s -> %x\n",
        mcdata.hit ? "HIT " : "MISS",
        addr, vsize, index, entry->dump(), mcdata.value);

    return mcdata.is_taken();
}


mc_entry_t*
Minicache::allocate(uint64_t addr, int16_t *index_, bool isstore){
    // Allocate entry
    int16_t index = get_evict(addr);
    mc_entry_t *entry = &entries[index];

    // Can Alloc
    if (entry->lock){
        if (index_){
            *index_ = 0;
        }
        return nullptr;
    }

    // realloc entry
    entry->reallocate(MINICACHE_ADDR_F(addr, clsize), isstore);
    // Return index and entry
    if (index_){
        *index_ = index;
    }
    return entry;
}


#define FMT "@%08x, size=%1x, line[%2d] = %s <- %x\n"

void
Minicache::write_speculative(uint64_t addr, uint8_t vsize, uint64_t value,
    mc_inst_data_t &mcdata){
    mc_entry_t *entry;
    int16_t index = get_index(addr);
    if (index != -1){ // HIT Select index entry
        entry = &entries[index];
    } else { // MISS evict entry -> allocate
        entry = allocate(addr, &index, true);
        if (!entry){ // Fail allocate
            return;
        }
    }

    mcdata.locktag = true;
    entry->lock += 1;

    entry->write_speculative(addr, vsize, (uint8_t*)&value);
    DPRINTF(Cva6Minicache, "write specu  " FMT,
        addr, vsize, index, entry->dump(), value);
    update_policy();
}

void
Minicache::clear(uint64_t addr){
    int16_t index = get_index(addr);
    if (index != -1){
        entries[index].clear();
        DPRINTF(Cva6Minicache, "clear line[%2d] = %s\n",
            index, entries[index].dump());
    }
}

void
Minicache::load_issue(uint64_t addr, mc_inst_data_t &mcdata){
    mc_entry_t *entry;
    int16_t index = get_index(addr);
    if (index == -1){ // Only allocate if tag is missing
        entry = allocate(addr, NULL, false);
    } else {
        entry = &entries[index];
    }
    if (!entry){ // Fail allocate
        return;
    }
    mcdata.locktag = true;
    entry->lock += 1;
}

void
Minicache::writeback_load_speculative(uint64_t addr, uint8_t vsize,
    uint8_t *data, mc_inst_data_t &mcdata)
{
    if (!mcdata.locktag){
        return;
    }
    mc_entry_t *entry;
    int16_t index = get_index(addr);
    if (index == -1){ // Tag is absent
        fatal("Tag must be here");
        return;
    }
    entry = &entries[index];
    // if (entry->lock){
    //     return;
    // }
    entry->write_x_speculative(addr, vsize, data);
    DPRINTF(Cva6Minicache, "write specuX " FMT,
        addr, vsize, index, entry->dump(), 0xd00d);
}

void
Minicache::commit(uint64_t addr, uint8_t vsize, uint8_t *data,
    bool isstore, mc_inst_data_t &mcdata){

    if (!isstore){ // Some statistics
        if (mcdata.hit){
            stats.hit += 1;
            stats.hit_corr += mcdata._need_corr;
            stats.hit_wbs  += mcdata._is_wbs;
            stats.hit_salloc += mcdata._is_salloc;
        } else{
            stats.miss += 1;
            stats.miss_tag += mcdata._miss_tag;
        }
    }

    if (!mcdata.locktag){ // Ignore unlocked reqs
        return;
    }

    mc_entry_t *entry;
    int16_t index = get_index(addr);
    if (index == -1){ // Tag is absent
        fatal("Tag must be here");
        return; /* entries[get_evict(addr)].lock -= 1;*/
    }
    entry = &entries[index];
    entry->commit(addr, vsize, data, isstore);
    DPRINTF(Cva6Minicache, "commit " FMT,
        addr, vsize, index, entry->dump(), 0xdead);

    assert(entry->lock > 0);
    entry->lock -= 1;
}

void
Minicache::flush(){
    for (int i = 0; i < size; i++){
        mc_entry_t *e = &entries[i];
        e->flush();
        e->lock = 0;
        DPRINTF(Cva6Minicache, "flush line[%2d] = %s\n", i, e->dump());
    }
}


} // namespace cva6
} // namespace gem5
