/**
 * plugin.cc
 *
 *  Author:   Pierre Ravenel
 * Created:   04/11/2023
 **/

#include "cpu/cva6/plugin.hh"

#include <string>

namespace gem5 {
namespace cva6 {

void
PluginMemtrace::init(const std::string &path){
    // Get memtrace env var
    // env_p = std::getenv("MEMTRACEPATH");
    DPRINTF(Cva6Plugin, "register plugin: MEMTRACEPATH is %s\n", path);
    fatal_if(path == "", "No path provided\n");
    file.open(path, std::ios::out | std::ios::binary);
    fatal_if(!file.is_open(), "Invalid path\n");
}

void
PluginMemtrace::commit(Cva6DynInstPtr inst){
    // DPRINTF(Cva6Plugin, "DUDU !! : %s\n", *inst);
    // if (!file.is_open()){
    //     return;
    // }

    if (inst->isFault() || !inst->staticInst->isMemRef()){
        return;
    }

    typedef struct entry
    {
        uint64_t pc;
        uint64_t addr;
        uint64_t data;
        uint32_t size;
        uint32_t is_store;
    }entry_t;

    entry_t e = {
        inst->pc->instAddr(),
        inst->dreq->req->getVaddr(),
        inst->dreq->getData(),
        inst->dreq->req->getSize(),
        !inst->staticInst->isLoad()
    };

    file.write((char *)&e, sizeof(entry_t));
    file.flush();
    DPRINTF(Cva6Plugin, "DUMP: pc=%lx addr=%lx data=%lx isstore=%ld\n",
        e.pc, e.addr, e.data, e.is_store);
}


void
PluginSimpointBar::init(const BaseCva6CPUParams &params){
    if (!params.simpoint_start_insts.empty()) {
        is_enable = true;
        assert(params.simpoint_start_insts.size() == 2);
        simcpt_size = params.simpoint_start_insts[1];
    }
}
#include "sim/cur_tick.hh"

void
PluginSimpointBar::commit(Cva6DynInstPtr inst){
    uint64_t deltainst = simcpt_size/1000;
    if (is_enable) {
        if (cpt == 0){ // initialisation
            firstcycle = cpu.curCycle();
            oldcycle = cpu.curCycle();
        }
        cpt += 1;
        if (cpt % (deltainst) == 0){
            float ipc = (float)deltainst / (cpu.curCycle() - oldcycle);
            float ipcg =  (float)cpt / (cpu.curCycle() - firstcycle);
            oldcycle = cpu.curCycle();

            uint64_t tick = gem5::curTick();

            printf("%16ld: SIMCPT: %ld/%ld = %f :: ipc=%f, ipcg=%f\n",
                tick,
                cpt, simcpt_size, (float)cpt/simcpt_size,
                ipc, ipcg);
        }
    }
}

void
PluginVPP::commit(Cva6DynInstPtr inst){

    vp_inst_metadata_t vp_data;

    if (vp.isEnable() &&
        !inst->isFault() &&
        inst->staticInst->isLoad())
    {
        /* 0) Setup prediction */
        uint64_t pc = inst->vp_data._pc;
        uint64_t size = inst->vp_data.inst_mem_req_size;
        uint64_t imm = inst->vp_data.inst_mem_req_imm;
        vp_data.init(pc, size, imm);
        /* 1) Make prediction */
        vp.predict(&vp_data);
        /* 2) Update prediction at issue */
        uint64_t base_addr = inst->getSrcRegOperand(0);
        uint64_t eff_addr = inst->dreq->req->getVaddr();
        assert(eff_addr == base_addr + imm);
        vp.update_prediction_at_issue(&vp_data, base_addr);
        /* Commit */
        uint64_t real_val = inst->dreq->getData();
        vp.commit(pc, base_addr, size, real_val, &vp_data, true);
    }
    /* Commit F$*/
    if (vp.isEnable() &&
        !inst->isFault() &&
        inst->staticInst->isMemRef()
    ){
        Addr pc = inst->pc->instAddr();
        bool isload = inst->staticInst->isLoad();
        bool isamo = inst->staticInst->isAtomic();
        if (!vp_data.hit) { // Need update
        // TODO: do not flush when unbufferable
        if (isload){
            bool is_bufferable = inst->dreq->isCl();
            assert(!isamo);
            if (is_bufferable){
                vp.fake_cache_write(pc,
                    inst->dreq->getClVaddr(),
                    inst->dreq->getClSize(),
                    inst->dreq->getClData(),
                    isload, false);
            }
        } else {
            bool flush = isamo; // Amo must be executed
            uint64_t data = inst->dreq->getData();
            vp.fake_cache_write(pc,
                inst->dreq->req->getVaddr(),
                inst->dreq->req->getSize(),
                (uint8_t*)&data, isload, flush);
        }
        }
    }
}

void
PluginMCVP::commit(Cva6DynInstPtr inst){
    if (inst->isFault() || !inst->staticInst->isLoad()){
        return;
    }
    uint8_t vpp_hit = inst->vp_data._predperfect_valid;
    uint8_t vp_hit = inst->vp_data.hit; // && inst->vp_data.taken;
    uint8_t mc_hit = inst->mc_data.hit;
    switch ((mc_hit << 1) | (vp_hit)){
        case 0b00:
            stats.none += 1;
            break;
        case 0b01:
            stats.vp += 1;
            break;
        case 0b10:
            stats.mc += 1;
            break;
        case 0b11:
            stats.mcvp += 1;
            break;
        default:
            fatal("Unrecheable\n");
    }
    stats.mcnvpvpp += mc_hit && !vp_hit && vpp_hit;
    stats.nvpvpp += !mc_hit && !vp_hit && vpp_hit;
}


void
PluginGoodbadTrap::commit(Cva6DynInstPtr inst){
    if (passAddr == inst->pc->instAddr()){
        std::cout << "*** GOOD TRAP *** @0x"
                <<  std::hex << passAddr << std::endl;
        exitSimLoop("*** GOOD TRAP ***", 0);
    }
    if (failAddr == inst->pc->instAddr()){
        std::cout << "*** BAD TRAP *** @0x"
                << std::hex << failAddr << std::endl;
        exitSimLoop("*** BAD TRAP ***", 1);
    }
}


void
PluginMemConst::commit(Cva6DynInstPtr inst){
    if (inst->isFault() || !inst->staticInst->isMemRef()){
        return;
    }
    assert(inst->dreq);

    bool is_store = inst->staticInst->isStore();
    uint64_t addr = inst->dreq->req->getPaddr(); // DW
    if (!is_store){
        stats.req += 1;
    }

    for (int i = 0; i < PMC_RANGE; i++){
        uint64_t ppn = addr >> (3+i);
        std::map<uint64_t, std::map<uint64_t, uint64_t>> &mm = mem[i];
        // if (!mm.count(ppn)){
        //     mm[ppn] =
        // }
        std::map<uint64_t, uint64_t> &page = mm[ppn];

        if (!is_store){ // stats
            if (page.count(addr)){
                if (page[addr] != 0){ // Load -> Load
                    stats.reqllrange[i] += 1;
                }
            }
        }

        if (is_store){
            page.clear();
        } else {
            if (page.count(addr)){
                page[addr] += 1;
            } else {
                page[addr] = 1;
            }
        }
    }
}

} // namespace cva6
} // namespace gem5
