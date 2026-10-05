/**
 * plugin.cc
 *
 *  Author:   Pierre Ravenel
 * Created:   04/11/2023
 **/


#pragma once

#include <zlib.h>

#include <string>
#include <vector>

#include "base/callback.hh"
#include "base/named.hh"
#include "base/statistics.hh"
#include "base/time.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/hmp.hh"
#include "cpu/cva6/preschedulers/oracle.hh"
#include "cpu/cva6/pure_block.hh"
#include "debug/Cva6Plugin.hh"
#include "mem/packet.hh"
#include "sim/core.hh"

namespace gem5 {
namespace cva6 {

class Plugin : public Named
{
  protected:
    Cva6CPU &cpu;

  public:
    Plugin(const std::string &name, Cva6CPU &cpu_)
        : Named(name), cpu(cpu_) { ; }

    virtual void commit(Cva6DynInstPtr inst) = 0;
    virtual void finalyse() {}
};

class PluginChecker : public Plugin
{
  InfiniteMemory64 memcheck;
  ArchRegFile<char> rfinit;
  ArchRegFile<uint64_t> rf;
  public:
  PluginChecker(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Plugin(name, cpu_),
      memcheck("memcheck", cpu) {}
  void commit(Cva6DynInstPtr inst){
    /*  Mem Checker */
    if (!inst->isFault() && inst->staticInst->isMemRef()){
      uint64_t addr = inst->dreq->getPaddr();
      uint8_t size = inst->dreq->getSize();
      uint64_t value = inst->dreq->getData();
      if (inst->dreq->isBufferable()){ /* Bufferable load or store */
          if (inst->staticInst->isLoad()){ /* Load checker */
              bool isconst = memcheck.check_load(addr, size, value);
              inst->exec_data.is_const_load = isconst;
          } else { /* Update store*/
              bool indempotant = memcheck.check_store(addr, size, value);
              inst->exec_data.is_silent_store = indempotant;
          }
      } else { /* Not bufferable */
          memcheck.invalidate(addr);
      }
    }
    // Reg checker
    if (!inst->isFault()){
      // 0) Check src
      for (auto& reg: inst->regs_src_phy){
        if (reg.is_reg_dead){
          rfinit[reg] = false;
        }
        if (!rfinit[reg]){ // For simpoint
          rf[reg] = reg.value;
          // rfinit[reg] = true;
        }
        fatal_if(rf[reg] != reg.value,
          "reg %s must be equal to %lx not %lx\n",
          reg, rf[reg], reg.value);
      }
      // 1) Apply dsts
      for (auto& reg: inst->regs_dst_phy){
        rf[reg] = reg.value;
        rfinit[reg] = true;
      }
    }
  }
};

class PluginMemtrace : public Plugin
{
  protected:
    std::ofstream file;
  public:
    PluginMemtrace(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Plugin(name, cpu_)
    {
      init(params.plugin_memtrace_path);
    }
    void init(const std::string& path);
    void commit(Cva6DynInstPtr inst);

};

class PluginSimpointBar : public Plugin
{
  protected:
    size_t cpt = 0;
    size_t simcpt_size;
    bool is_enable = false;

    Cycles oldcycle;
    Cycles firstcycle;

    Time startTime;
    double oldtime;

  public:
    PluginSimpointBar(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Plugin(name, cpu_),
      oldcycle(0),
      firstcycle(0),
      startTime(true),
      oldtime(0.f)
    { init(params); }
    void init(const BaseCva6CPUParams &params);
    void commit(Cva6DynInstPtr inst);
};

class PluginVPP : public Plugin
{
  protected:
    VP &vp; /* Value predictor */
  public:
    PluginVPP(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Plugin(name, cpu_),
      vp(*vpinit(params.vpType, cpu.name() + ".vpp", cpu, params.vpSize))
    {}
    void commit(Cva6DynInstPtr inst);
};

class PluginMCVP : public Plugin
{
  protected:
    struct MCVPStats : public statistics::Group
    {
      /** Stats */
      statistics::Scalar none; // No one hit
      statistics::Scalar mc; // only mc hit
      statistics::Scalar vp; // only vp hit
      statistics::Scalar mcvp; // Both mc and vp hit

      statistics::Scalar mcnvpvpp; // MC ^ !vp ^ vpperfect
      statistics::Scalar nvpvpp; // !MC ^ !vp ^ vpperfect

      MCVPStats(BaseCPU &cpu) :
        statistics::Group(&cpu, "mcvp"),
        ADD_STAT(none, statistics::units::Count::get(),"none"),
        ADD_STAT(mc, statistics::units::Count::get(),"mc"),
        ADD_STAT(vp, statistics::units::Count::get(),"vp"),
        ADD_STAT(mcvp, statistics::units::Count::get(),"mcvp"),
        ADD_STAT(mcnvpvpp, statistics::units::Count::get(),"mcnvpvpp"),
        ADD_STAT(nvpvpp, statistics::units::Count::get(),"nvpvpp") { }
    } stats;
  public:
    PluginMCVP(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Plugin(name, cpu_), stats(cpu_) { }
    void commit(Cva6DynInstPtr inst);

};

#include "sim/sim_exit.hh"

class PluginGoodbadTrap : public Plugin
{
  protected:
    /** Termination */
    Addr passAddr;
    Addr failAddr;

  public:
    PluginGoodbadTrap(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Plugin(name, cpu_),
      passAddr(params.passAddr),
      failAddr(params.failAddr) { }
    void commit(Cva6DynInstPtr inst);

};

class PluginLambda : public Plugin
{
  protected:
    size_t n = 0;
    PureBlock handler;

  public:
    PluginLambda(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Plugin(name, cpu_),
      handler("lambda", cpu_, params) {
      // get a callback when we exit
      // registerExitCallback([this]() { handler.dump(); });
    }

  void commit(Cva6DynInstPtr inst){
    handler.commit(inst);
    n++;
    if (n % 1000000 == 0){
      // handler.dump();
    }
  }
};


class PluginHMP : public Plugin
{
  protected:
    HMP hmp;

  public:
    PluginHMP(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Plugin(name, cpu_),
      hmp(name, cpu_, params) {
    }

  void commit(Cva6DynInstPtr inst){
    if (!inst->isFault() && inst->staticInst->isLoad()){
      bool prediction = hmp.predict(inst);
      hmp.commit(inst, prediction);
    }
  }
};

#define PMC_RANGE 20

class PluginMemConst : public Plugin
{
  protected:
    std::map<uint64_t, std::map<uint64_t, uint64_t>> mem[PMC_RANGE];
    // PPN -> <Addr, Valid> -> Valid
    struct MemStats : public statistics::Group
    {
      /** Stats */
      statistics::Scalar req;
      statistics::Scalar reqll;
      statistics::Scalar reqsl;

      statistics::Vector reqllrange;

      MemStats(BaseCPU &cpu) :
        statistics::Group(&cpu, "memc"),
        ADD_STAT(req, statistics::units::Count::get(),"req"),
        ADD_STAT(reqll, statistics::units::Count::get(),"reqll"),
        ADD_STAT(reqsl, statistics::units::Count::get(),"reqsl"),
        ADD_STAT(reqllrange,statistics::units::Count::get(),"")
      {
        reqllrange.init(PMC_RANGE);
      }
    } stats;

  public:
    PluginMemConst(const std::string &name,
      Cva6CPU &cpu_,
      const BaseCva6CPUParams &params) :
      Plugin(name, cpu_), stats(cpu_) { }
    void commit(Cva6DynInstPtr inst);

};

class PluginTageHC : public Plugin
{
  protected:
  struct Stats : public statistics::Group
  {
      /* DIRECT CONDITIONAL : tagged table entries */
      statistics::Scalar TConfMatch;
      statistics::Scalar TNoConfNoMatch;
      statistics::Scalar TConfNoMatch;
      statistics::Scalar TNoConfMatch;
      /* DIRECT CONDITIONAL : Bimotag table */
      statistics::Scalar BConfMatch;
      statistics::Scalar BNoConfNoMatch;
      statistics::Scalar BConfNoMatch;
      statistics::Scalar BNoConfMatch;

      /* DIRECT UNCONDITIONNAL : No misspred */
      /* [ ... ] */

      /* INDIRECT UNCONDITIONAL : Indirect predictor */
      statistics::Scalar UConfMatch;
      statistics::Scalar UNoConfNoMatch;
      statistics::Scalar UConfNoMatch;
      statistics::Scalar UNoConfMatch;
      /* INDIRECT UNCONDITIONAL : Ras for returns */
      statistics::Scalar RConfMatch;
      statistics::Scalar RNoConfNoMatch;
      statistics::Scalar RConfNoMatch;
      statistics::Scalar RNoConfMatch;

      /* INDIRECT CONDITIONAL : Not in risc-v ? */
      /* [ ... ] */

      statistics::Scalar req;
      statistics::Scalar missCond;
      statistics::Scalar missUncond;
      statistics::Scalar miss;
      statistics::Scalar hitcond_missaddr;

      Stats(Cva6CPU &cpu) :
        statistics::Group(&cpu, "tagehc"),
        ADD_STAT(TConfMatch, ""),
        ADD_STAT(TNoConfNoMatch, ""),
        ADD_STAT(TConfNoMatch, ""),
        ADD_STAT(TNoConfMatch, ""),
        ADD_STAT(BConfMatch, ""),
        ADD_STAT(BNoConfNoMatch, ""),
        ADD_STAT(BConfNoMatch, ""),
        ADD_STAT(BNoConfMatch, ""),
        ADD_STAT(UConfMatch, ""),
        ADD_STAT(UNoConfNoMatch, ""),
        ADD_STAT(UConfNoMatch, ""),
        ADD_STAT(UNoConfMatch, ""),
        ADD_STAT(RConfMatch, ""),
        ADD_STAT(RNoConfNoMatch, ""),
        ADD_STAT(RConfNoMatch, ""),
        ADD_STAT(RNoConfMatch, ""),
        ADD_STAT(req, ""),
        ADD_STAT(missCond, ""),
        ADD_STAT(missUncond, ""),
        ADD_STAT(miss, ""),
        ADD_STAT(hitcond_missaddr, "") {  }
    } stats;
  public:
    PluginTageHC(const std::string &name,
      Cva6CPU &cpu_, const BaseCva6CPUParams &params) :
      Plugin(name, cpu_), stats(cpu) {}

  void commit(Cva6DynInstPtr inst){
    if (inst->isFault() || !inst->staticInst->isControl()){
      return;
    }
    bool is_addr_unmatch = inst->triedToPredict &&
                          *inst->predictedTarget != *inst->pc_next;
    stats.missCond += is_addr_unmatch && inst->staticInst->isCondCtrl();
    stats.missUncond += is_addr_unmatch && inst->staticInst->isUncondCtrl();
    stats.miss += is_addr_unmatch;
    stats.req ++;
    bool conf = inst->isHighConf;
    if (inst->staticInst->isReturn()){
      bool match = !is_addr_unmatch;
      stats.RConfMatch += conf && match;
      stats.RNoConfNoMatch += !conf && !match;
      stats.RConfNoMatch += conf && !match;
      stats.RNoConfMatch += !conf && match;
    } else if (inst->staticInst->isCondCtrl()){
      bool match = inst->predictedTaken == inst->pc_next_taken;
      // if (!is_addr_unmatch && !match){
      //   fatal("Cannot match addr with dir missmatch !\n");
      // }
      // Must BE 0 with secure frontend
      stats.hitcond_missaddr += match && is_addr_unmatch;
      if (inst->predFromBim){
        // To discriminate the size of CPT from
        // the BIM to the tagges tables
        stats.BConfMatch += conf && match;
        stats.BNoConfNoMatch += !conf && !match;
        stats.BConfNoMatch += conf && !match;
        stats.BNoConfMatch += !conf && match;
      } else {
        stats.TConfMatch += conf && match;
        stats.TNoConfNoMatch += !conf && !match;
        stats.TConfNoMatch += conf && !match;
        stats.TNoConfMatch += !conf && match;
      }
      // mat[conf][match] ++;
      // printf("TP=%ld, TN=%ld :: FP=%ld, FN=%ld\n",
      //     mat[1][1], mat[0][0], mat[1][0], mat[0][1]);
    } else if (inst->staticInst->isUncondCtrl() &&
               inst->staticInst->isIndirectCtrl()){
      bool match = !is_addr_unmatch;
      stats.UConfMatch += conf && match;
      stats.UNoConfNoMatch += !conf && !match;
      stats.UConfNoMatch += conf && !match;
      stats.UNoConfMatch += !conf && match;
    }
    /* else direct uncond : cannot miss */
  }
};


class PluginScheduler : public Plugin
{
  protected:
    std::deque<OracleBench*> benchs;

  public:
    PluginScheduler(const std::string &name,
    Cva6CPU &cpu,
    const BaseCva6CPUParams &p) :
    Plugin(name, cpu) {
        // std::vector<int> sizes = {8, 16, 32, 64, 128, 256, 512};
        // std::vector<int> sizes = {128}; // debug
        // for (int x : sizes){
        //}
        benchs.push_back(new OracleBench(cpu, "sched128", 128, 0, 0));
        benchs.push_back(new OracleBench(cpu, "sched128ser", 128, 1, 1));
    }

    void commit(Cva6DynInstPtr inst){
        for (auto &bench: benchs){
            bench->tick(inst);
        }
    }
};

class PluginCBP2025 : public Plugin
{
  protected:
    // std::ofstream out;
    gzFile out;// = gzopen("trace.bin.gz", "wb");
  public:
    PluginCBP2025(const std::string &name,
    Cva6CPU &cpu,
    const BaseCva6CPUParams &p);
    void commit(Cva6DynInstPtr inst);
    void finalyse() override {
      gzclose(out);
    }
};


class Plugins
{
  /** Plugins */
  std::list<Plugin*> plugins;
  public:
  Plugins(const std::string &name_,
        Cva6CPU &cpu,
        const BaseCva6CPUParams &params);

  void commit(Cva6DynInstPtr inst){
    for (Plugin *plugin: plugins){
      plugin->commit(inst);
    }
  }

  void finalyse(){
    std::cout << "Callback before gem5 exit!" << std::endl;
    for (Plugin *plugin: plugins){
      plugin->finalyse();
    }
  }

};

} // namespace cva6
} // namespace gem5
