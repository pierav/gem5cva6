#include "cpu/cva6/dyn_inst.hh"

#include <iomanip>
#include <sstream>

// #include "arch/isa.hh"
#include "cpu/base.hh"
#include "cpu/cva6/exec_context.hh"
#include "cpu/cva6/exec_context_speculative.hh"
#include "cpu/cva6/exec_context_static.hh"
#include "cpu/null_static_inst.hh"
#include "cpu/reg_class.hh"
#include "debug/Cva6Execute.hh"
#include "enums/OpClass.hh"

namespace gem5 {
namespace cva6 {

const InstSeqNum InstId::firstFetchSeqNum;
const InstSeqNum InstId::firstExecSeqNum;

std::ostream &
operator <<(std::ostream &os, const InstId &id)
{
    os << id.fetchSeqNum << '.' << id.execSeqNum;
    return os;
}

Cva6DynInstPtr Cva6DynInst::bubbleInst = []() {
    auto *inst = new Cva6DynInst();
    inst->incref();
    return inst;
}();

bool
Cva6DynInst::isLastOpInInst() const
{
    assert(staticInst);
    return !(staticInst->isMicroop() && !staticInst->isLastMicroop());
}


Cva6DynInst::inststate_t::inststate_t(const Cva6DynInstPtr inst){
    assert(inst);
  fatal_if(inst->isBubble(), "Cannot be bubble\n");

  /* If fault bypass */
  if (inst->isFault()){
    return;
  }

  // First arg is PC
  fatal_if(inst->regs_src_phy.size() > 3, "Too mush src\n");
  fatal_if(inst->regs_dst_phy.size() > 1, "Too mush dst\n");

  int k = 0;
  regs[k++] = inst->pc->instAddr();

  for (auto &reg: inst->regs_src_phy){
    regs[k++] = reg.value;
  }
  for (auto &reg: inst->regs_dst_phy){
    regs[k++] = reg.value;
  }
}


#include <iomanip>

#if 0
void regDump(RegId reg, std::ostream &ss){
    if (reg.classValue() == IntRegClass){
        ss << " x";
    } else if (reg.classValue() == FloatRegClass){
        ss << "fp";
    } else {
        ss << "??";
    }
    ss << std::dec << std::setfill('0') << std::setw(2) << reg.index();
}
#endif


#define RST  "\x1B[0m"
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define KBLU  "\x1B[34m"
#define KMAG  "\x1B[35m"
#define KCYN  "\x1B[36m"
#define KWHT  "\x1B[37m"

#ifdef XXX
#define PIPE_START "\xCB"
#define PIPE_END "\xCA"
#define PIPE "\xBA"
#define PIPENO "."
#else
#define PIPE_START "S"
#define PIPE_END "E"
#define PIPE "|"
#define PIPENO "."
#endif

std::ostream&
Cva6DynInst::basedump(std::ostream &os) const {
  if (isBubble()){
    os << "bubble";
  } else {
    os << "0x" << std::hex << pc->instAddr() << std::dec;
    os << " sn:" << id.fetchSeqNum;
    os << ": ";
    if (isFault()){
      os << "F: " << getFault()->name();
    } else if (staticInst) {
      #if 0
      #define COLOR "\x1B[1;36m"
      /* CUSTOM */
      if (l_data.is_predicted_first){
          os << COLOR PIPE_START " " RST;
      } else if (l_data.is_predicted_last){
          os << COLOR PIPE_END " " RST;
      } else if (l_data.is_predicted){
          os << COLOR PIPE " " RST;
      } else {
          os << PIPENO " " ;
      }
      #endif
      os << std::setw(30) << std::left
      << staticInst->disassemble(pc->instAddr());
      //   << "Flags=";
      // staticInst->printFlags(os, ",");
      //   ->getName();
      if (issue_start_ts){
        for (auto &reg: regs_dst_phy){
          reg.dumpWithValue(os);
          os << ' ';
        }
        for (auto &reg: regs_src_phy){
          reg.dumpWithValue(os);
          os << ' ';
        }
      }
    }
  }
  return os;
}

std::ostream &operator <<(std::ostream &os, const Cva6DynInst &inst){
    return inst.basedump(os);
}

Fault
Cva6DynInst::initiateMemRead(Addr addr, unsigned int size,
                    Request::Flags flags,
                    const std::vector<bool>& byte_enable)
{
    dreq = new DTLBRequest(
        *cpu, pcState().instAddr(),
        addr, size, flags, byte_enable,
        NULL, NULL, nullptr, BaseMMU::Read
    );
    DPRINTF(Cva6X, "initiateMemRead %s\n", dreq->name());
    return NoFault;
}


Fault
Cva6DynInst::writeMem(uint8_t *data, unsigned int size, Addr addr,
            Request::Flags flags, uint64_t *res,
            const std::vector<bool>& byte_enable) {

    if (data == NULL) {
        assert(flags & Request::STORE_NO_DATA);
    }

    dreq = new DTLBRequest(
        *cpu, pcState().instAddr(),
        addr, size, flags, byte_enable,
        data, res, nullptr, BaseMMU::Write
    );

    DPRINTF(Cva6X, "writeMem %s\n", dreq->name());
    return NoFault;
}

Fault
Cva6DynInst::initiateMemAMO(Addr addr, unsigned int size,
            Request::Flags flags, AtomicOpFunctorPtr amo_op) {

    dreq = new DTLBRequest(
        *cpu, pcState().instAddr(),
        addr, size, flags, std::vector<bool>(size, true),
        NULL, NULL, std::move(amo_op), BaseMMU::Write
    );

    DPRINTF(Cva6X, "initiateMemAMO %s\n", dreq->name());
    return NoFault;
}

/***** Effectives functions *****/

void
Cva6DynInst::executeInitiateStatic(){
    if (staticInst->isMemRef()){
        ExecContextStatic context(this->static_data);
        staticInst->initiateAcc(&context, traceData);
    }
}

void
Cva6DynInst::executeInitiate(){
    DPRINTF(Cva6X, "executeInitiate... %s\n", *this);
    if (isFault()) { // Nothing to do
        DPRINTF(Cva6X, "isFault... %s\n", *this);
        return;
    }
    // Setup default next pc. Jump instructions modify this value.
    set(pc_next, pc);
    // Initate memory references
    if (staticInst->isMemRef()){
        DPRINTF(Cva6X, "isMemRef... %s\n", *this);
        ExecContextSpeculative context(this);
        Fault fault = staticInst->initiateAcc(&context, traceData);
        if (fault != NoFault){
            setFaultEx(fault);
        } else {
            assert(dreq); // initiateAcc must create dreq
            if (traceData){
                traceData->setMem(
                    dreq->req->getVaddr(),
                    dreq->req->getSize(),
                    dreq->req->getFlags());
            }
        }
    }
}

void
Cva6DynInst::executeComplete(){
    DPRINTF(Cva6X, "executeComplete... %s\n", *this);
    if (isFault()) { // Fault
        return;
    }

    if (staticInst->isNonSpeculative()){
        // We must wait commit to update state
        return;
    }

    if (staticInst->isMemRef()){ // Memory Fault
        if (dreq->fault != NoFault){
            setFaultEx(dreq->fault);
            return;
        }
    }

    // Compute result in speculative context
    ExecContextSpeculative context(this);
    if (staticInst->isMemRef()) {
        if (staticInst->isLoad() ||
            staticInst->isAtomic() ||
            staticInst->isStoreConditional())
        { // Non bufferable
            PacketPtr packet = dreq->pkt;
            setFaultEx(staticInst->completeAcc(packet,
                &context, traceData));
            // delete dreq->pkt;
            // dreq->pkt = NULL;
            // untrackDreq();
            // delete dreq;
        }
    } else {
        // DPRINTF(Cva6Execute, "speculative Committing inst: %s\n", *this);
        setFaultEx(staticInst->execute(&context, traceData));
    }
}

Fault
Cva6DynInst::executeCommit(Cva6CPU &cpu, SimpleThread &thread){
    DPRINTF(Cva6X, "executeCommit... %s\n", *this);
    if (isFault()) {
        if (traceData) {
            traceData->setFaulting(true);
        }
        getFault()->invoke(thread.getTC());
    } else {
        if (staticInst->isNonSpeculative()){ // Non speculative
            // Ensure execute()
            panic_if(staticInst->isMemRef(), "Cannot be memref");
            // Execute in real context
            ExecContext context(cpu, thread, this);
            setFaultEx(staticInst->execute(&context, traceData));
            if (isFault()) {
                if (traceData) {
                    traceData->setFaulting(true);
                }
                getFault()->invoke(thread.getTC());
                return getFault();
            }
        } else { // Speculative execution:
            // Copy destination regs
            for (unsigned int i = 0; i < staticInst->numDestRegs(); i++) {
                RegId reg = staticInst->destRegIdx(i);
                if (reg.classValue() != InvalidRegClass){
                    // Copy reg
                    RegVal regv = getDstRegOperand(i);
                    thread.setReg(reg, regv);
                }
            }

            // Write CSR if needed
            for (auto const& p: ex_csrs){
                thread.setMiscReg(p.first, p.second);
            }
        }
        // compute next pc
        staticInst->advancePC(*pc_next);
        thread.pcState(*pc_next);
    }
    return getFault();
}

Cva6DynInst::~Cva6DynInst()
{
    if (traceData)
        delete traceData;
    untrackDreq();
}


} // namespace cva6
} // namespace gem5
