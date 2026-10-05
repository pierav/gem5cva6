import m5
from m5.objects.BaseCPU import BaseCPU
from m5.objects.BranchPredictor import *
from m5.objects.Cache import Cache
from m5.params import *
from m5.proxy import *


class BaseCva6CPU(BaseCPU):
    type = "BaseCva6CPU"
    cxx_header = "cpu/cva6/cpu.hh"
    cxx_class = "gem5::Cva6CPU"

    @classmethod
    def memory_mode(cls):
        return "timing"

    @classmethod
    def require_caches(cls):
        return True

    @classmethod
    def support_take_over(cls):
        return True

    dcache = Param.Cache(Cache(), "The L1 data cache")
    icache = Param.Cache(Cache(), "The L1 instructions cache")

    fetch2InputBufferSize = Param.Unsigned(
        2, "Size of input buffer to Fetch2 in cycles-worth of insts."
    )

    branchPred = Param.BranchPredictor(
        LocalBP(numThreads=Parent.numThreads), "Branch Predictor"
    )

    storeSetSize = Param.Unsigned(1024, "StoreSet Size")

    # LSU
    lsuSQCWidth = Param.Unsigned(16, "Store Queue Commit size")
    lsuSQSWidth = Param.Unsigned(16, "Store Queue Speculative size")
    loadPerCycle = Param.Unsigned(4, "Load issued per cycle (max)")

    frontLatency = Param.Unsigned(1, "Decode stage latency")

    mcSize = Param.Unsigned(16, "Minicache size; 0 for disable")
    # VP
    vpSize = Param.Unsigned(2048, "Value prediction size; 0 for disable")
    vpType = Param.Unsigned(0, "Value prediction type")
    vpFlush = Param.Unsigned(0, "Flush instead of replay")

    # DPE
    dpeTestMode = Param.Unsigned(
        0, "Performs loads to validate deterministics predictions"
    )
    dpeIgnore = Param.Unsigned(0, "Do not delay loads predictions")

    passAddr = Param.Addr(0x0, "Good trap address")
    failAddr = Param.Addr(0x0, "Bad trap address")

    plugin_memtrace_path = Param.String("", "Memtrace")
    m5out = Param.String("", "m5 rundir")

    # IEW
    sbSize = Param.Unsigned(32, "Scoreboard size")
    sbOoO = Param.Unsigned(0, "Enable OoO IQ")
    sbFSC = Param.Unsigned(0, "Enable FSC cpu model")

    # Ex
    flushAtExecute = Param.Unsigned(0, "Flush at Execute")

    numROBEntries = Param.Unsigned(64, "Rob size")

    issueWidth = Param.Unsigned(4, "Issue width")
    commitWidth = Param.Unsigned(4, "Commit Width")

    # Rename
    renameSize = Param.Unsigned(64, "Rename size")
    renameIncArchReg = Param.Unsigned(0, "Sb arch(0) or Full RR(1)")
    renameFreeRegDead = Param.Unsigned(0, "Free Dead registers")
    renameSpecRelease = Param.Unsigned(1, "Release PReg in the frontend")
    renameSpecReleasePC = Param.Unsigned(0, "Release PReg after precommit")
    renameSpecRelaseInplace = Param.Unsigned(1, "Allow in place reuse of prs")
    renameSerrAllocFirst = Param.Unsigned(1, "Serr alloc first and not argmax")

    # Lambda
    userelf = Param.String("", "Main user elf")
    lltSize = Param.Unsigned(0, "Last Lamdba Table Size")

    # Scheduler
    schedType = Param.Unsigned(0, "Scheduler type. (0 for no scheduler)")
    schedSize = Param.Unsigned(128, "Scheduler size")
    schedWidth = Param.Unsigned(4, "Scheduler width")
    schedRegBarrier = Param.Unsigned(16, "Inter RRB delay")
    schedDisableRB = Param.Unsigned(0, "Disable all register barrier")
    # Exstage
    oracleEarlyCommit = Param.Unsigned(0, "Oracle that allow early commit")

    def addCheckerCpu(self):
        print("Checker not yet supported by Cva6CPU")
        exit(1)
