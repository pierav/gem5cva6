/**
 * plugin.cc
 *
 *  Author:   Pierre Ravenel
 * Created:   04/11/2023
 **/


#pragma once

#include <string>
#include <vector>

#include "base/named.hh"
#include "base/statistics.hh"
#include "base/time.hh"
#include "cpu/cva6/dyn_inst.hh"
#include "cpu/cva6/pure_block.hh"
#include "cpu/cva6/scheduler.hh"
#include "debug/Cva6Plugin.hh"
#include "debug/Cva6Sched.hh"
#include "debug/Cva6SchedSched.hh"
#include "mem/packet.hh"

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


class PluginScheduler : public Plugin
{
  protected:
  struct Stats : public statistics::Group
  {
    statistics::Scalar req;
    statistics::Scalar rescheduled;

    Stats(Cva6CPU &cpu) :
      statistics::Group(&cpu, "sched"),
      ADD_STAT(req, ""),
      ADD_STAT(rescheduled, "")
    { }
  } stats;

  /* Statistics only */
  uint64_t bbcnt = 0;
  StreamAnalyser instats;
  StreamAnalyser instatsnobr;
  StreamAnalyser outstats;
  std::deque<Cva6DynInstPtr> fifo;

  /* The scheduler */
  BaseScheduler &scheduler;

  /* Renamming */
  PhysicalRegAllocator regalloc;

  public:
  PluginScheduler(const std::string &name,
    Cva6CPU &cpu,
    const BaseCva6CPUParams &p) :
    Plugin(name, cpu),
    stats(cpu),
    instats(cpu, "stream.in", false),
    instatsnobr(cpu, "stream.innobr", true),
    outstats(cpu, "stream.out", SCHED_IGNORE_BRANCH),
    scheduler(*new SchedulerPierreMichaud(name, cpu, p)),
    regalloc(64) { }

  /* Insert instruction in scheduler */
  void push(Cva6DynInstPtr inst){
    inst->bb_idx = bbcnt;
    bbcnt += !inst->isFault() && inst->staticInst->isControl();

    /* Rename instruction : default is no renamming */
    BinaryRegisterFile rf;
    for (uint8_t i = 0; i < inst->numSrcRegs(); i++) {
      RegId regid = inst->srcRegIdx(i);
      if ((regid.classValue() != InvalidRegClass) && !rf.isSet(regid)){
        PhysicalReg reg(id2i(regid));
        reg.is_reg_dead = inst->exec_data.is_reg_dead[i];
        inst->regs_src_phy.push_back(reg);
        rf.set(regid);
      }
    }
    rf.clear();
    for (uint8_t i = 0; i < inst->numDstRegs(); i++) {
      RegId regid = inst->dstRegIdx(i);
      if ((regid.classValue() != InvalidRegClass) && !rf.isSet(regid)){
        inst->regs_dst_phy.push_back(PhysicalReg(id2i(regid)));
        rf.set(regid);
      }
    }
    /* Annotate missing Reg Dead */
    for (auto& reg: inst->regs_src_phy){
      if (rf.isSetRaw(reg.virt_reg_idx)){ /* Rf contains rd regs */
        reg.is_reg_dead = true;
      }
    }

    /* Rename */
    regalloc.rename(inst);

    /* Push in scheduler */
    // DPRINTF(Cva6SchedSched, "Push: %s\n", dumpInstPreg(inst));
    scheduler.push(inst);
  }

  void commit(Cva6DynInstPtr inst){

    fifo.push_back(inst);
    push(inst); // Push Inst in scheduler
    if (fifo.size() > NB_INFLIGHTS){
      Cva6DynInstPtr unschedisnt = fifo.front();
      fifo.pop_front();
      Cva6DynInstPtr schedinst = scheduler.pop(); // Pop inst from scheduler

      instats.commit(unschedisnt);
      instatsnobr.commit(unschedisnt);

      schedinst->delta = outstats.commit(schedinst);

      DPRINTF(Cva6SchedSched, "Schedule: %s (+%d)\n",
        dumpInstPreg(schedinst), schedinst->delta);

    }
  }
};


} // namespace cva6
} // namespace gem5

