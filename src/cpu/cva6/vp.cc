/**
 * vp.cc
 *
 *  Author:   Pierre Ravenel
 * Created:   04/07/2023
 **/

#include "cpu/cva6/vp.hh"

#include <iomanip>

#include "base/named.hh"
#include "debug/Cva6VP.hh"

namespace gem5 {
namespace cva6 {

enum VPTYPE
{
  // base predictor
  ASTRA = 0,
  // Valud predictor
  LVP = 1,
  Str2D = 2,
  DFCM = 3,

  // Addr+f$
  ALVP = 101,
  AStr2D = 102,
  ADFCM = 103,
  PAP = 104,
  CAP = 105,

  ASTRA_C = 200,
};

BaseAddrPred *init_ad_vp(int64_t type, size_t size){
    switch (type % 100){
        case 0:
            return new BaseAddrPredASTRA(size);
        case 1:
            return new BaseAddrPredLVP(size);
        // case 2:
        //     break;
        case 3:
            return new BaseAddrPredDFCM(size);
        case 4:
            return new BaseAddrPredPAP(size);
        case 5:
            return new BaseAddrPredCAP(size);
    }
    fatal("Invalid AD type: %d\n", type);
    return NULL;
}

VP* init_value_vp(int64_t type, const std::string &name,
 BaseCPU &cpu, size_t size){
    switch(type % 100){
        case 0:
        case 1:
            return new VP_LVP(name, cpu, size);
        case 2:
            return new VP_Str2D(name, cpu, size);
        case 3:
            return new VP_DFCM(name, cpu, size);
    }
    fatal("Invalid VP type: %d\n", type);
    return NULL;
}

VP* vpinit(int64_t type, const std::string &name, BaseCPU &cpu,
size_t size){
    uint64_t vp_type = type / 100;
    switch (vp_type){
        case 0: // Base VP
            return init_value_vp(type, name, cpu, size);
        case 1: // Addr -> Value + Determinism
            return new VP_VXXX(name, cpu, size,
            *init_ad_vp(type, size), false);
        case 2: // Addr -> Value + Determinism + Compressed
            return new VP_VXXX_C(name, cpu, size,
             *init_ad_vp(type, size), false);
        case 3: // Addr -> Value + Determinism + Compressed + METALVP
            return new VP_VXXX_C(name, cpu, size,
             *init_ad_vp(type, size), true);
    }
    fatal("Invalid VP : %d\n", type);
    return nullptr;
}



uint64_t foldn(uint64_t v, uint8_t n){
    uint64_t res = 0;
    while (v){
        res ^= v;
        v >>= n;
    }
    return res;
}

static inline int log2i(int x) {
    assert(x > 0);
    return sizeof(int) * 8 - __builtin_clz(x) - 1;
}

uint64_t foldlogn(uint64_t v, size_t size){
    return (foldn(v, log2i(size)) % size);
}

int64_t fixstride(int64_t stride){
    if (stride > (1<<16)){
        stride &= ((int64_t)1<<16) - 1;
        return stride;
    } else if (-stride > (1 << 16)){
        stride |= ~(((int64_t)1 << 16) -1);
    }
    return stride;
}



uint64_t
VP::compute_key(uint64_t shr){
    return (foldn(shr, log2i(size)) % size);
}


void
VP::commitaccount(vp_inst_metadata_t *res, uint64_t real_val){
    bool taken = res->value_taken;
    bool ready = res->value_ready;
    bool pred_valid = (res->pred_val == real_val);

    stats.value_hit += pred_valid;
    stats.value_hit_ready += pred_valid && ready;
    stats.value_miss_ready += !pred_valid && ready;
    stats.value_hit_taken += pred_valid && taken;
    stats.value_miss_taken += !pred_valid && taken;
    stats.req += 1;

    if (taken){
        if (pred_valid){
            DPRINTF(Cva6VP, "VP %016lx: HIT__PRED\n", res->_pc);
        } else {
            DPRINTF(Cva6VP, "VP %016lx: MISS_PRED\n", res->_pc);
        }
        static int hits[2];
        hits[pred_valid] += 1;
        DPRINTF(Cva6VP, "VP : #(t^h)=%d, #(t^!h)=%d :: %f\n",
            hits[1], hits[0], (float)hits[1]/(hits[1] + hits[0]));
    }
    DPRINTF(Cva6VP, "VP %016lx: commit_ V [%d] %s: pred %x real %x\n",
        res->_pc, taken, pred_valid ? "OK" : "KO", res->pred_val, real_val);
}

void
VP_VXXX::predict_base_addr(vp_inst_metadata_t *res){
    // Predict base addr
    res->addr_ready = AP.predict(res->_pc, res);
    DPRINTF(Cva6VP, "VP %016lx: predict @=%lx [%d]\n",
        res->_pc, res->t1_addr, res->addr_ready);
}

bool
VP_VXXX::predict_readfc(vp_inst_metadata_t *res, bool atcommit){
    uint64_t pc = res->_pc;
    uint16_t rsize = res->inst_mem_req_size;


    /* Read fake cache and store aliaser */
    bool is_fc_match = fake_cache_read(pc, res->eff_addr, rsize, res);
    bool fc_valid = fc_sa_is_dep(res->eff_addr);

    // History
    // res->_pap_history = hist;
    // hist = hist << 1 | (last_pc > pc);
    // last_pc = pc;
    // bool conf_hist = ht[(pc ^ res->_pap_history) % size].conf == CONF_MAX;

    res->pred_val_vastra_valid = // !res->_is_taga &&
        // res->t1_isconf && // Addr conf
        // conf_hist &&
        // is_t2_conf &&
        fc_valid && // No Store disrupting at predict
        is_fc_match;

    // LVP
    if (meta_lvp_enable){
        uint64_t tag = (pc >> 1);
        lvp_entry_t *lve = &lvt[tag%size];
        res->pred_val_lvp = lve->value;
        res->pred_val_lvp_valid = lve->conf.valid();
    }

    // if (!atcommit){ /* Push inflight */
    //     DPRINTF(Cva6VP, "VP %016lx: predict [%d][%d&%d&%d]"
    //         "%016x @ %16lx\n",
    //         pc, res->pred_val_vastra_valid,
    //         res->t1_isconf, fc_valid, is_fc_match,
    //         res->pred_val_vastra, res->eff_addr);
    // }
    return res->value_ready;
}

bool
VP_VXXX::update_prediction_at_issue(vp_inst_metadata_t *res,
    uint64_t base_addr){
    // 1) Check is the predicted address is valid
    bool matchaddrtag = (base_addr % size) == (res->t1_addr % size);
    matchaddrtag = base_addr == res->t1_addr;
    if (!matchaddrtag){
        DPRINTF(Cva6VP, "VP %016lx: remove issue b@ %lx -> %lx\n",
        res->_pc, res->t1_addr, base_addr);
    }

    // res->pred_val_lvp_valid = false;
    res->value_ready = false;
    res->hit = false;
    res->pred_val = res->pred_val_vastra;
    // return false;

    if (res->pred_val_vastra_valid && matchaddrtag){ // Deter VASTRA
        res->hit = true;
        res->value_ready = true;
        res->_hit_deter = true;
        res->pred_val = res->pred_val_vastra;
        DPRINTF(Cva6VP, "VP %016lx: issue DETER @=%lx : %lx\n",
            res->_pc, res->t1_addr, res->pred_val);
    }
    else if (res->pred_val_lvp_valid) {
        res->value_ready = true;
        res->pred_val = res->pred_val_lvp;
        DPRINTF(Cva6VP, "VP %016lx: issue PROBA : %lx\n", res->_pc,
        res->pred_val);
    } else {
        DPRINTF(Cva6VP, "VP %016lx: issue NOTHING\n", res->_pc);
    }

    return res->value_ready;
}

void
VP_VXXX::store_issued(uint64_t pc, uint64_t eff_addr){
    int cpt = fc_sa_increment(eff_addr);
    // DPRINTF(Cva6VP, "VP %016lx:    LOCK @%016lx : %d\n",
    //    pc, eff_addr, cpt);
}

void
VP_VXXX::store_commit(uint64_t pc, uint64_t eff_addr){
    int cpt = fc_sa_decrement(eff_addr);
    // DPRINTF(Cva6VP, "VP %016lx: UNLOCK @%016lx : %d\n",
    //    pc, eff_addr, cpt);
}

bool
VP_VXXX::commit(uint64_t pc, uint64_t addr, uint16_t rsize, uint64_t real_val,
           vp_inst_metadata_t *res, bool is_load){
    stats._hitaddr_exp += res->t1_addr == addr;
    stats._hitaddrt1_exp += res->_pred_addr_t1 == addr;

    // Check prediction
    bool pred_valid = (res->pred_val == real_val);
    bool addr_valid = (res->t1_addr == addr);
    res->_pred_valid = pred_valid;
    // res->_predperfect_valid = perfect_res.pred_val == real_val;

    bool taken = res->addr_taken;
    bool ready = res->addr_ready;
    bool valid = addr_valid;

    stats.addr_hit += valid;
    stats.addr_hit_ready += valid && ready;
    stats.addr_miss_ready += !valid && ready;
    stats.addr_hit_taken += valid && taken;
    stats.addr_miss_taken += !valid && taken;

    bool addr_hit_ready = valid && ready;
    stats._hit_wbs += addr_hit_ready && res->_is_wbs;
    stats._hit_fca += addr_hit_ready && res->_is_fca;
    stats._hit_stra += addr_hit_ready && res->_is_stra;
    stats._hit_deter += addr_hit_ready && res->_hit_deter;
    stats._hit_taga += addr_hit_ready && res->_is_taga;
    stats._hit_stra_fca += addr_hit_ready && res->_is_stra && res->_is_fca;

    DPRINTF(Cva6VP, "VP %016lx: commit_ @ [%d] %s: pred %x real %x\n",
        res->_pc, taken, valid ? "OK" : "KO", res->t1_addr, addr);

    commitaccount(res, real_val);

    // if (res->addr_ready){
    //     static int hitsa[2];
    //     hitsa[addr_valid] += 1;
    //     DPRINTF(Cva6VP, "VP : #(t^@)=%d, #(t^!@)=%d :: %f\n",
    //         hitsa[1], hitsa[0], (float)hitsa[1]/(hitsa[1] + hitsa[0]));
    // }

    // LVP Commit
    uint64_t tag = (pc >> 1);
    lvp_entry_t *lve = &lvt[tag%size];
    lve->value = real_val;
    lve->conf.update_conf(res->pred_val_lvp == real_val);

    // AP Commit
    AP.commit(pc, addr, res);
    AP.update_conf(pc, valid, res);

    // bool addridxok = (res->t1_addr % size) == (addr % size);
    // bool pred_valid_vastra = res->pred_val_vastra == real_val;
    // if (addridxok){ // Addr conditioning
    //     AP.update_conf(pc, pred_valid_vastra, res);
    // }
    return 0;
}

void
VP::fake_cache_setconf(uint64_t addr, bool valid){
    vp_fc_entry_t *avec = fc_get_entry(addr);
    if (valid && avec->conf < CONF_MAX){
        avec->conf += 1;
    }
    if (!valid && avec->conf > 0){
        avec->conf -= 1;
    }
}

bool
VP::fake_cache_write(uint64_t pc, uint64_t addr, uint16_t rsize,
    uint8_t *data, bool is_load, bool flush)
{
    uint64_t tag2 = fc_tag(addr);
    uint16_t offset = fc_offset(addr);
    assert((offset + rsize) <= fclinesize);
    vp_fc_entry_t *avec = fc_get_entry(addr);

    // Debug
    uint8_t need_update = memcmp(avec->data + offset, data, rsize) == 0;


    if (flush){ // flush line
        avec->tag = 0;
        avec->valid =false;
        memset(avec->data, 0xde, fclinesize);
    } else {
        // update when store and tag match or
        // or wh~en load with invalid tag. Is the tag is valid, the data
        // was already good
        bool update = (avec->tag != tag2 &&  is_load) ||
                      (avec->tag == tag2 && !is_load);
        if (update){
            memcpy(avec->data + offset, data, rsize);
            avec->tag = tag2;
            avec->valid = true;
            // Stats metadata
            avec->_write_by_store = !is_load;
            avec->_pc_writter = pc;
            avec->_updated = need_update;
            std::stringstream ss;
            ss << std::hex << std::setfill('0');
            for (int i = rsize-1; i >= 0; i--) {
                ss << std::hex << std::setw(2) << static_cast<int>(data[i]);
            }
            DPRINTF(Cva6VP, "VP %016lx: f$ write @ %016lx : "
                        "%s#%d>>%d [%s,%s]\n",
                        pc, addr, ss.str(), rsize, offset,
                        flush ? "FLUSH": "W", is_load ? "LD" : "SD");
        }
    }
    return true;
}

bool
VP::fake_cache_read(uint64_t pc, uint64_t addr, uint16_t rsize,
    vp_inst_metadata_t *res){
    uint16_t offset = fc_offset(addr);

    if ((offset + rsize) > fclinesize){ // Bad addr for the size
        return false;
    }
    assert((fc_offset(addr) + rsize) <= fclinesize);

    vp_fc_entry_t *avec = fc_get_entry(addr);
    bool valid = avec->valid;
    bool tagmatch = avec->tag == fc_tag(addr);
    bool align = (fc_offset(addr) % rsize) == 0;

    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = rsize-1; i >= 0; i--) {
        ss << std::hex << std::setw(2) << static_cast<int>(avec->data[i]);
    }
    DPRINTF(Cva6VP, "VP %016lx: f$ read  @ %016lx : "
                    "%s #%d (tag(%s)=%lx)\n",
        pc, addr, ss.str(), rsize,
        tagmatch ? "THIT" : "TMISS", avec->tag << log2i(fclinesize));

    res->fc_isconf = avec->conf == CONF_MAX;
    memcpy(&res->pred_val_vastra, avec->data + offset, rsize);
    res->_is_wbs = avec->_updated && avec->_write_by_store;
    res->_is_fca = avec->_updated && avec->_pc_writter != res->_pc;
    return valid && tagmatch && align;
}

void
VP::fake_cache_clear_by_addr(uint64_t up_addr){
    for (int i = 0; i < fcsize; i++){
        if (!avt[i].valid){
            continue;
        }
        if ((avt[i].tag >> (20 - log2i(fclinesize))) == up_addr){
            // Clear entry if tag match
            DPRINTF(Cva6VP, "VP: clear tag @: %lx\n", avt[i].tag << (3));
            avt[i].tag = 0;
            avt[i].valid = false;
            //TODO: Need valid ?
        }
    }
}


vp_inflight_store_cpt_t *
VP::fc_sa_get(uint64_t addr){
    return &store_aliaser[foldlogn(fc_tag(addr), store_aliaser_size)];
}

bool
VP::fc_sa_is_dep(uint64_t addr){
    return fc_sa_get(addr)->cpt == 0;
}

int
VP::fc_sa_increment(uint64_t addr){
    fc_sa_get(addr)->cpt += 1;
    return fc_sa_get(addr)->cpt;
}

int
VP::fc_sa_decrement(uint64_t addr){
    fc_sa_get(addr)->cpt -= 1;
    return fc_sa_get(addr)->cpt;
}

void
VP::fc_sa_clear(void){
    for (int i = 0; i < store_aliaser_size; i++){
        store_aliaser[i].cpt = 0; // Reset inflight stores
    }
}

void
VP::flush(){
    DPRINTF(Cva6VP, "VP ******************** FLUSH ********************\n");
    fc_sa_clear();
}


} // namespace cva6
} // namespace gem5
