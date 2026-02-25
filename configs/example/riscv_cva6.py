#
# riscv_cva6.py
# author: Pierre Ravenel (pravenel@kalrayinc.com)
#  brief: main python launcher
#
import sys

print(sys.version)

import argparse
import sys

import m5
from m5.defines import buildEnv
from m5.objects import *
from m5.util import (
    addToPath,
    fatal,
    warn,
)
from m5.util.fdthelper import *

# TODO !!!
addToPath("../")
from os import path

# addToPath("/nfs/home/pravenel/gem7/configs/")
from common import Options
from common.SysPaths import binary

parser = argparse.ArgumentParser()
# Options.addCommonOptions(parser)
# Options.addFSOptions(parser)

parser.add_argument("--kernel", action="store", type=str)
parser.add_argument("--issueWidth", action="store", type=int, default=4)

# Prefetcher size
parser.add_argument("--pfSize", action="store", type=str, default="64")
parser.add_argument("--pf2Size", action="store", type=str, default="256")

STORE_TRUE = {"action": "store_true"}
DEFAULT = lambda x: {"default": x}

common_config = {"numROBEntries": DEFAULT(64)}

cva6_config = {
    **common_config,
    "lsuSQCWidth": DEFAULT(32),  # SQCommit size
    "lsuSQSWidth": DEFAULT(32),  # SQSpecualtive size
    "loadPerCycle": DEFAULT(2),  # Number of load per cycle
    "frontLatency": DEFAULT(0),  # Branch pred miss extra penality
    "mcSize": DEFAULT(0),  # The minicache size
    "vpSize": DEFAULT(0),  # The Value Predictor Size
    "vpType": DEFAULT(0),  # The Value Predictor Type
    "vpFlush": STORE_TRUE,  # Flush on missprediction
    "dpeTestMode": STORE_TRUE,
    "dpeIgnore": STORE_TRUE,
    "lltSize": DEFAULT(0),  # Enable lambda things ...
    "sbSize": DEFAULT(32),  # Scoreboard size
    "sbOoO": STORE_TRUE,
    "sbFSC": STORE_TRUE,
    "schedType": DEFAULT(0),  # The type of scheduler used in frontend
    "schedSize": DEFAULT(128),
    "schedWidth": DEFAULT(4),
    "schedRegBarrier": DEFAULT(16),
    "schedDisableRB": DEFAULT(0),
    "userelf": DEFAULT(""),  # User elf for symbols only
    "renameSize": DEFAULT(16),  # Default 16 extra reg
    "renameIncArchReg": DEFAULT(0),  # Include ARCH in PRF
    "renameFreeRegDead": DEFAULT(0),  # Free reg dead (bugs)
    "renameSpecRelease": DEFAULT(0),  # Speculative Release
    "renameSpecReleasePC": DEFAULT(0),  # SR at pre commit
    "renameSpecRelaseInplace": DEFAULT(1),
    "renameSerrAllocFirst": DEFAULT(0),
    "oracleEarlyCommit": DEFAULT(0),
    "flushAtExecute": STORE_TRUE,  # Flush at execute
    "storeSetSize": DEFAULT(1024),
}

o3_config = {
    **common_config,
    "numIQEntries": DEFAULT(32),
    "numPhysIntRegs": DEFAULT(180),
    "numPhysFloatRegs": DEFAULT(168),
    "numPhysVecRegs": DEFAULT(168),
    "LQEntries": DEFAULT(32),
    "SQEntries": DEFAULT(32),
}

parser.add_argument(
    "--freq",
    type=str,
    default="1GHz",
    help="Core frequency",
)

parser.add_argument("--plugmemtrace", action="store_true")

for k, v in {**cva6_config, **o3_config}.items():
    parser.add_argument("--" + k, **v)

# CPU
parser.add_argument("--cpu", choices=["cva", "o3", "amo"], default="cva")


# IO
parser.add_argument("--disk", action="store", type=str, help="Virtio disk")

# L1
parser.add_argument(
    "--l1dsize",
    action="store",
    type=str,
    default="64kB",
    help="L1 data cache size. Default: 64kB.",
)
parser.add_argument(
    "--l1dlat",
    action="store",
    type=int,
    default=4,
    help="L1 data latency. Default: 4",
)
parser.add_argument("--l2dlat", default=10, help="L2 data latency")


# Checkpoints
parser.add_argument(
    "--cpt",
    action="store",
    type=str,
    default=None,
    help="restore checkpoint dir",
)
parser.add_argument(
    "--simcpt",
    action="store",
    type=str,
    default=None,
    help="Restore simpoint and do measurements",
)
parser.add_argument("--exitOnCpt", action="store_true")

# Simpoint options (Pass 1 : collect statistics)
parser.add_argument(
    "--simpoint-profile",
    action="store_true",
    help="Enable basic block profiling for SimPoints",
)
parser.add_argument(
    "--simpoint-interval",
    type=int,
    default=10000000,
    help="SimPoint interval in num of instructions",
)
# Simpoint options (Pass 2 : take simpoints checkpoints)
parser.add_argument(
    "--take-simpoint-checkpoints",
    action="store",
    type=str,
    help="<simpoint file,weight file,interval-length,warmup-length>",
)


# Run duration options
parser.add_argument(
    "-I",
    "--maxinsts",
    action="store",
    type=int,
    default=None,
    help="Total number of instructions to simulate",
)


# FUS
class CVA6_ALU(FUDesc):
    opList = [OpDesc(opClass="IntAlu", opLat=1)]
    count = 4


class CVA6_MUL(FUDesc):
    opList = [OpDesc(opClass="IntMult", opLat=3)]
    count = 2


class CVA6_SERDIV(FUDesc):
    opList = [OpDesc(opClass="IntDiv", opLat=8, pipelined=False)]
    count = 1


class CVA6_FPU(FUDesc):
    opList = [
        OpDesc(opClass="FloatAdd", opLat=3),
        OpDesc(opClass="FloatCmp", opLat=3),
        OpDesc(opClass="FloatCvt", opLat=3),
        OpDesc(opClass="FloatMult", opLat=3),
        OpDesc(opClass="FloatMultAcc", opLat=3),
        OpDesc(opClass="FloatMisc", opLat=3),
    ]
    count = 4


class CVA6_FPU_DIVSQRT(FUDesc):
    opList = [
        OpDesc(opClass="FloatDiv", opLat=18, pipelined=False),
        OpDesc(opClass="FloatSqrt", opLat=18, pipelined=False),
    ]
    count = 2


args = parser.parse_args()
print(args)

CONFIG_USE_O3 = False
CONFIG_USE_CVA6 = False
CONFIG_USE_ATOMIC = False

# Fixup
if args.userelf == "":
    args.userelf = args.kernel

CONFIG_USE_CVA6 = args.cpu == "cva"
CONFIG_USE_O3 = args.cpu == "o3"
CONFIG_USE_ATOMIC = args.cpu == "amo"


if args.simpoint_profile or args.take_simpoint_checkpoints:
    print("/!\\ Switch to atomic CPU for simpoint profiling /!\\")
    CONFIG_USE_ATOMIC = True

CONFIG_USE_CACHES = 1
CONFIG_USE_CACHE_L2 = 1
CONFIG_USE_PTW_CACHES = True  # Needed only for buzybox ?!
CONFIG_USE_DDR = True

# GO fast
if CONFIG_USE_ATOMIC:
    CONFIG_USE_CACHES = False
    CONFIG_USE_PTW_CACHES = False
    CONFIG_USE_DDR = False

# CONFIG_USE_PTW_CACHES = False
# CONFIG_USE_DDR = False
# CONFIG_USE_CACHE_L2 = False

# Memory mapping
BASE_ADDR_BOOTROM = 0x10000
addr_range_bootrom = AddrRange(start=BASE_ADDR_BOOTROM, size="32KiB")
BASE_ADDR_MEM = 0x80000000
addr_range_mem = AddrRange(start=BASE_ADDR_MEM, size="16GiB")

# 8000_0000 : BFFF_FFFF : 1 GB ram
# C000_0000 : FFFF_FFFF : 1 GB drive
BASE_ADDR_DRIVE = 0x180000000
# addr_range_drive = AddrRange(start=BASE_ADDR_DRIVE, size='4GiB')

# 1G: (4000_0000)16

#   memory@80000000 {
#     device_type = "memory";
#     reg = <0x0 0x80000000 0x0 0x80000000>;
#   };
#   chosen {
#     stdout-path = &SERIAL0;
#     bootargs = "console=ttyS0 earlycon";
#   };


def generateChosen():
    node = FdtNode("chosen")
    node.append(FdtPropertyStrings("stdout-path", ["/soc/uart@10000000"]))
    node.append(FdtPropertyStrings("bootargs", ["console=ttyS0 earlycon"]))
    return node


def generateMemNode(state, mem_range):
    node = FdtNode("memory@%x" % int(mem_range.start))
    node.append(FdtPropertyStrings("device_type", ["memory"]))
    node.append(
        FdtPropertyWords(
            "reg",
            state.addrCells(mem_range.start)
            + state.sizeCells(mem_range.size()),
        )
    )
    return node


def generateDrive(state, mem_range):
    node = FdtNode("reserved-memory")
    node.append(state.addrCellsProperty())
    node.append(state.sizeCellsProperty())
    node.append(FdtProperty("ranges"))

    node_part = FdtNode("region@%x" % int(mem_range.start))
    node_part.append(FdtPropertyStrings("label", ["gem5drive"]))
    node_part.append(FdtProperty("no-map"))

    node_part.append(
        FdtPropertyWords(
            "reg",
            state.addrCells(mem_range.start)
            + state.sizeCells(mem_range.size() / 4),
        )
    )
    node.append(node_part)

    # node.append(FdtPropertyStrings("device_type", ["memory"]))

    #     reserved-memory {
    #    #address-cells = <2>;
    #    #size-cells = <2>;
    #    ranges;

    #    reserved: buffer@0 {
    #       no-map;
    #       reg = <0x0 0x70000000 0x0 0x10000000>;
    #    };
    # };

    # node = FdtNode("memory")
    # local_state = FdtState(addr_cells=2, size_cells=2)

    # node.append(FdtPropertyWords("reg",
    #     state.addrCells(mem_range.start) +
    #     state.sizeCells(mem_range.size() /4) ))

    # node_part = FdtNode("region@%x" % int(mem_range.start))
    # node_part.append(FdtPropertyStrings("label", ["gem5drive"]))
    # node_part.append(FdtProperty("linux,reserve-region"))
    # node_part.append(FdtPropertyWords("reg",
    #     state.addrCells(mem_range.start) +
    #     state.sizeCells(mem_range.size() /4) ))

    # node.append(node_part)
    return node


def generateDtb(system, dtb_file):
    """
    Autogenerate DTB. Arguments are the folder where the DTB
    will be stored, and the name of the DTB file.
    """
    state = FdtState(addr_cells=2, size_cells=2, cpu_cells=1)
    root = FdtNode("/")
    root.append(state.addrCellsProperty())
    root.append(state.sizeCellsProperty())
    root.appendCompatible(["riscv-virtio"])

    root.append(generateChosen())
    # chosen =  FdtNode('chosen')
    # chosen.append(FdtPropertyStrings("stdout-path", "&SERIAL0"))
    # chosen.append(FdtPropertyStrings("bootargs", "console=ttyS0 earlycon"))
    # root.append(chosen)

    sections = [*system.cpu, system.platform]

    for section in sections:
        for node in section.generateDeviceTree(state):
            if node.get_name() == root.get_name():
                root.merge(node)
            else:
                root.append(node)
    # Generate memories
    for mem_range in system.mem_ranges:
        root.append(generateMemNode(state, mem_range))

    plic = system.platform.plic

    # Generate drive
    # root.append(generateMemNode(state, addr_range_drive))
    # root.append(generateDrive(state, addr_range_drive))
    disk = system.disk
    disk_node = disk.generateBasicPioDeviceNode(
        state, "virtio_mmio", disk.pio_addr, disk.pio_size
    )
    disk_node.append(FdtPropertyWords("interrupts", [disk.interrupt_id]))
    disk_node.append(FdtPropertyWords("interrupt-parent", state.phandle(plic)))
    disk_node.appendCompatible(["virtio,mmio"])
    root.append(disk_node)

    fdt = Fdt()
    fdt.add_rootnode(root)
    fdt.writeDtsFile(path.join(m5.options.outdir, "cva6.dts"))
    fdt.writeDtbFile(dtb_file)


def write_file(file, data):
    with open(file, "w") as f:
        f.write(data)


def generate_bootrom(system, base_addr=0):
    dtb_file = path.join(m5.options.outdir, "cva6.dtb")
    bootrom_file = path.join(m5.options.outdir, "bootrom.S")
    linker_file = path.join(m5.options.outdir, "linker.ld")
    elf_file = path.join(m5.options.outdir, "bootrom.elf")
    # print("Generate dtb...")
    generateDtb(system, dtb_file=dtb_file)

    bootrom = f"""
.section .text.start, "ax", @progbits
.globl _start
_start:
  li s0, 1
  slli s0, s0, 31
  csrr a0, mhartid
  la a1, _dtb
  jr s0

.section .text.hang, "ax", @progbits
.globl _hang
_hang:
  csrr a0, mhartid
  la a1, _dtb
1:
  wfi
  j 1b

.section .rodata.dtb, "a", @progbits
.globl _dtb
.align 5, 0
_dtb:
.incbin "{dtb_file}"
"""
    write_file(bootrom_file, bootrom)

    linker = """
SECTIONS
{
    ROM_BASE = 0x10000;

    . = ROM_BASE;
    .text.start : { *(.text.start) }
    . = ROM_BASE + 0x40;
    .text.hang : { *(.text.hang) }
    . = ROM_BASE + 0x80;
    .rodata.dtb : { *(.rodata.dtb) }
}
"""
    write_file(linker_file, linker)
    import os
    import subprocess

    # RISCV_DIR = os.getenv("RISCV")
    # if RISCV_DIR is None:
    #     print("RISCV env var is not set")
    #     exit(1)
    # cc = f"{RISCV_DIR}/bin/riscv64-unknown-elf-gcc"
    cc = "riscv64-unknown-elf-gcc"
    cc = "/crex/proj/uart/ravenelp/toolchain15.1/bin/riscv64-unknown-elf-gcc"
    incs = f"-I{m5.options.outdir}"
    cflags = "-march=rv32i_zicsr -mabi=ilp32 -nostdlib -static -std=gnu99"
    ldflags = f"-T{linker_file} -Wl,--no-gc-sections,-e_start"
    cmd = f"{cc} {incs} {cflags} -o {elf_file} {bootrom_file} {ldflags}"
    # try:
    #     subprocess.run("echo $RISCV", shell=True, check=True)
    # except Exception as e:
    #     print(f"Subprocess error !: {e}")
    #     exit(1)
    # print("Subprocess...")
    try:
        # print(cmd)
        subprocess.run(cmd, shell=True, check=True)
    except Exception as e:
        print(f"Subprocess error !: {e}")
        exit(1)
    # print("DONE")
    # subprocess.run(f"readelf -h {elf_file}", shell=True)
    return elf_file


l1delay = 0


class L1Cache(Cache):
    """Simple L1 Cache with CVA6 values"""

    tag_latency = 0
    data_latency = 0
    response_latency = 0
    # mshrs = 1
    tgts_per_mshr = 32
    # replacement_policy = RandomRP()
    # clusivity = 'mostly_incl'
    # write_buffers = 0

    def __init__(self, options=None):
        super().__init__()
        pass

    def connectBus(self, bus):
        """Connect this cache to a memory-side bus"""
        self.mem_side = bus.cpu_side_ports

    def connectCPU(self, cpu):
        """Connect this cache's port to a CPU-side port
        This must be defined in a subclass"""
        raise NotImplementedError


class L1ICache(L1Cache):
    """Simple L1 instruction cache with CVA6 values"""

    size = "32kB"
    assoc = 4
    # is_read_only = True
    response_latency = 0  # 3x5cycles
    mshrs = 16  #

    def __init__(self, opts=None):
        super().__init__(opts)

    def connectCPU(self, cpu):
        """Connect this cache's port to a CPU icache port"""
        self.cpu_side = cpu.icache_port


class L1DCache(L1Cache):
    """Simple L1 data cache with CVA6 values"""

    size = args.l1dsize
    assoc = 8
    # writeback_clean = True WTF is this ?
    mshrs = 16  # 1 store, 1 load + 1 ???
    # write_buffers = 1024

    # demand_mshr_reserve = 512 # MSHRs reserved for demand access
    # tgts_per_mshr = 256 # Max number of accesses per MSHR

    data_latency = args.l1dlat - 1
    tag_latency = args.l1dlat - 1
    response_latency = 1

    def __init__(self, opts=None):
        super().__init__(opts)

    def connectCPU(self, cpu):
        """Connect this cache's port to a CPU dcache port"""
        self.cpu_side = cpu.dcache_port


class L2Cache(Cache):
    size = "256kB"
    assoc = 8
    tag_latency = args.l2dlat
    data_latency = args.l2dlat
    response_latency = 1
    mshrs = 20
    tgts_per_mshr = 12

    def __init__(self, opts=None):
        super().__init__()

    def connectCPUSideBus(self, bus):
        self.cpu_side = bus.mem_side_ports

    def connectMemSideBus(self, bus):
        self.mem_side = bus.cpu_side_ports


def createHiFivePlatform(system):
    # Main bus without delays, datasize 64bits
    lat = 0
    system.membus = SystemXBar(
        frontend_latency=lat,
        forward_latency=lat,
        response_latency=lat,
        header_latency=lat,
        width=64,
    )
    system.membus.badaddr_responder = BadAddr()
    system.membus.badaddr_responder.warn_access = "BAD ADDR ACCESS"
    system.membus.default = system.membus.badaddr_responder.pio
    system.membus.snoop_filter = NULL
    # system.system_outgPCaoing_bridge = OutgoingRequestBridge()
    system.system_port = system.membus.cpu_side_ports
    # system.system_outgoing_bridge.port

    # Ram
    if CONFIG_USE_DDR:
        system.mem_ctrl = MemCtrl()
        system.mem_ctrl.dram = DDR4_2400_4x16()
        system.mem_ctrl.dram.range = addr_range_mem
        # Useless
        # system.mem_ctrl.dram.device_size = "16GiB"
        system.mem_ctrl.dram.image_file = args.kernel
        system.mem_ctrl.static_frontend_latency = "0ns"
        system.mem_ctrl.static_backend_latency = "0ns"
    else:
        system.mem_ctrl = SimpleMemory()
        system.mem_ctrl.range = addr_range_mem
        system.mem_ctrl.image_file = args.kernel
        system.mem_ctrl.latency = "0ns"

    if CONFIG_USE_ATOMIC:
        system.mem_ctrl.port = system.membus.mem_side_ports
    else:
        system.rambridge = Bridge(delay="0ns")
        system.rambridge.cpu_side_port = system.membus.mem_side_ports
        system.rambridge.ranges = [addr_range_mem]
        # Single request memory
        system.rambridge.req_size = 64
        system.rambridge.resp_size = 64
        system.mem_ctrl.port = system.rambridge.mem_side_port

    # system.mem_ctrl.command_window = '1ns'
    # system.mem_ctrl.dram.

    system.rombridge = Bridge(delay="0ns")
    system.rombridge.cpu_side_port = system.membus.mem_side_ports
    system.rombridge.ranges = [addr_range_bootrom]
    system.rombridge.req_size = 64
    system.rombridge.resp_size = 64

    # Bootrom
    system.bootrom = SimpleMemory()
    system.bootrom.range = addr_range_bootrom
    # system.bootrom.latency = 1
    system.bootrom.port = system.rombridge.mem_side_port

    # system.memory_outgoing_bridge = OutgoingRequestBridge()
    # system.memory_outgoing_bridge.port = system.membus.mem_side_ports

    for cpu in system.cpu:
        if CONFIG_USE_CACHES:
            # Create an L1 instruction and data cache
            cpu.icache = L1ICache()
            cpu.dcache = L1DCache()
            if int(args.pfSize):
                cpu.dcache.prefetcher = StridePrefetcher()
                cpu.dcache.prefetcher.table_entries = args.pfSize
                # cpu.dcache.prefetcher.latency = 0
                # cpu.dcache.prefetcher.throttle_control_percentage = 40
                # cpu.dcache.prefetcher.degree = 16
                # cpu.dcache.prefetch = SignaturePathPrefetcher()

            # cpu.dcache.prefetch_on_access = True
            # Connect the instruction and data caches to the CPU
            cpu.icache.connectCPU(cpu)
            cpu.dcache.connectCPU(cpu)
            if CONFIG_USE_CACHE_L2:  # L2
                # create L2
                system.l2bus = L2XBar()
                system.l2cache = L2Cache()
                if int(args.pf2Size):
                    p = AMPMPrefetcher()
                    p.ampm.access_map_table_entries = args.pf2Size
                    system.l2cache.prefetcher = p

                system.l2cache.connectCPUSideBus(system.l2bus)
                system.l2cache.connectMemSideBus(system.membus)
                # system.l2cache.prefetcher = BOPPrefetcher()

                cpu.icache.connectBus(system.l2bus)
                cpu.dcache.connectBus(system.l2bus)
            else:
                # Hook the CPU ports up to the bus
                cpu.icache.connectBus(system.membus)
                cpu.dcache.connectBus(system.membus)
        else:
            system.membus.cpu_side_ports = cpu.icache_port
            system.membus.cpu_side_ports = cpu.dcache_port

        # Branch prediction
        cpu.branchPred = TAGE()
        # cpu.branchPred.tage = TAGE_SC_L_TAGE()
        # cpu.branchPred = LocalBP()
        # cpu.branchPred.localCtrBits = 2
        # cpu.branchPred.localPredictorSize = 128 * 2 # x2 localCtrBits
        # cpu.branchPred.BTBEntries = 32
        # cpu.branchPred.BTBTagSize = 16
        # cpu.branchPred.RASSize = 2**2
        # cpu.branchPred.instShiftAmt = 0

        # cpu.fetch1FetchLimit = 1

        cpu.createThreads()

        if CONFIG_USE_PTW_CACHES:
            cpu.itlbcache = L1DCache()
            cpu.itlbcache.connectBus(system.membus)
            cpu.dtlbcache = L1DCache()
            cpu.dtlbcache.connectBus(system.membus)
            cpu.mmu.connectWalkerPorts(
                cpu.itlbcache.cpu_side, cpu.dtlbcache.cpu_side
            )
        else:
            cpu.mmu.connectWalkerPorts(
                system.membus.cpu_side_ports, system.membus.cpu_side_ports
            )

    system.platform = HiFive()
    # Must be in src/dev/serial/uart
    # assert system.platform.uart.register_width == 4
    # assert system.platform.uart.pio_size == 8*4
    # system.platform.uart.pio_size = 8*4 # x4 Bytes
    # system.platform.uart.register_width = 4 # Bytes

    system.platform.pci_host.pio = system.membus.mem_side_ports

    system.platform.rtc = RiscvRTC(frequency=Frequency("10MHz"))
    system.platform.clint.int_pin = system.platform.rtc.int_pin

    # Add virtIO Disk
    if args.disk:
        print(f"Setup virtio disk {args.disk}")
        # image = CowDiskImage(
        #     child=RawDiskImage(read_only=True), read_only=False
        # )
        # image.child.image_file = args.disk
        image = RawDiskImage(read_only=True, image_file=args.disk)
        # "/work1/SPECRUN/speccpu2017ref.ext4.img"

        vio = VirtIOBlock(image=image)
        # system.disk.vio.image = image
    else:
        vio = VirtIODummyDevice()

    disk = RiscvMmioVirtIO(
        vio=vio,
        interrupt_id=0x8,
        pio_size=4096,
        pio_addr=0x480000000,
    )
    system.disk = disk
    disk_range = AddrRange(disk.pio_addr, size=disk.pio_size)

    # Ranges + pma checker config
    maxaddr = 0xFFFFFFFFFFFFFFFF
    uncacheable = list(AddrRange(0, maxaddr).exclude([addr_range_mem]))
    iorange = [
        *system.platform._on_chip_ranges(),
        *system.platform._off_chip_ranges(),
        disk_range,
    ]

    pma_checker = PMAChecker(uncacheable=uncacheable)
    system.cpu[0].mmu.pma_checker = pma_checker

    # IOBUS: memory -> ios
    system.iobus = IOXBar()
    system.bridge = Bridge(delay="0ns")  # Bridge(delay='38000ns')
    system.bridge.mem_side_port = system.iobus.cpu_side_ports
    system.bridge.cpu_side_port = system.membus.mem_side_ports
    system.bridge.ranges = iorange

    # Connect
    system.disk.pio = system.iobus.mem_side_ports
    system.platform.setNumCores(1)
    system.platform.attachOnChipIO(system.iobus)
    system.platform.attachOffChipIO(system.iobus)
    system.platform.attachPlic()


def good_bad_trap(elfname):
    if elfname is None:
        return {}
    try:
        import lief

        binary = lief.parse(elfname)
    except:
        return {}
    try:
        return {
            "passAddr": binary.get_function_address("pass"),
            "failAddr": binary.get_function_address("fail"),
        }
    except:
        pass
    try:
        # print("Try pass/fail...")
        return {
            "passAddr": binary.get_symbol("pass").value,
            "failAddr": binary.get_symbol("fail").value,
        }
    except:
        pass
    try:
        # print("Try shutdown/panic...") # TODO multiple elf
        return {
            "passAddr": 0,
            "failAddr": binary.get_function_address("panic"),
        }
    except:
        pass
    return {"passAddr": 0, "failAddr": 0}


print("*** Create system...")
system = System()
system.clk_domain = SrcClockDomain()
system.clk_domain.clock = args.freq  # "1GHz"  # '50MHz'
system.clk_domain.voltage_domain = VoltageDomain()
system.mem_ranges = [addr_range_mem]
system.cache_line_size = 64  # 128 // 8

if not CONFIG_USE_ATOMIC:
    cpuConfig = good_bad_trap(args.kernel)
    system.mem_mode = "timing"
    if CONFIG_USE_O3:
        # cpuConfig["fetchBufferSize"] = 16 # Bytes; same as I$
        cpuConfig["backComSize"] = 32
        cpuConfig["forwardComSize"] = 32
        pipewidth = args.issueWidth
        cpuConfig["numThreads"] = 1
        cpuConfig["commitWidth"] = pipewidth
        cpuConfig["squashWidth"] = 4096
        cpuConfig["issueWidth"] = pipewidth
        cpuConfig["dispatchWidth"] = pipewidth
        cpuConfig["wbWidth"] = pipewidth
        cpuConfig["decodeWidth"] = 8
        cpuConfig["fetchWidth"] = 8
        cpuConfig["renameToIEWDelay"] = 1

        # cpuConfig["commitToRenameDelay"] = 0
        # cpuConfig["renameToIEWDelay"] = 1
        # cpuConfig["trapLatency"] = 0
        for k in o3_config:
            cpuConfig[k] = vars(args)[k]

        # FUList = [ IntALU(),
        #            IntMultDiv(),
        #            FP_ALU(),
        #            FP_MultDiv(),
        #            ReadPort(count=2),
        #            SIMD_Unit(),
        #            PredALU(),
        #            WritePort(count=2),
        #            RdWrPort(count=0),
        #            IprPort() ]
        FUList = [
            CVA6_ALU(),
            CVA6_MUL(),
            CVA6_SERDIV(),
            CVA6_FPU(),
            CVA6_FPU_DIVSQRT(),
            ReadPort(count=4),
            WritePort(count=4),
            RdWrPort(count=0),
            IprPort(),
            SIMD_Unit(),
        ]

        cpuConfig["fuPool"] = FUPool(FUList=FUList)

        system.cpu = [RiscvO3CPU(cpu_id=i, **cpuConfig) for i in range(1)]
    else:
        for k in cva6_config:
            cpuConfig[k] = vars(args)[k]
        pipewidth = args.issueWidth
        cpuConfig["issueWidth"] = pipewidth
        cpuConfig["commitWidth"] = pipewidth
        if args.plugmemtrace:
            path = path.join(m5.options.outdir, "memtrace.bin")
            cpuConfig["plugin_memtrace_path"] = path
        system.cpu = [RiscvCva6CPU(cpu_id=i, **cpuConfig) for i in range(1)]

else:
    system.cpu = [RiscvAtomicSimpleCPU(cpu_id=i) for i in range(1)]

# system.cpu[0].ArchISA.enable_Zicbom_fs = False
# system.cpu[0].ArchISA.enable_Zicboz_fs = False
# system.cpu[0].ArchISA.enable_rvv = False

createHiFivePlatform(system)


print("*** Generate bootrom...")
# bbin = binary(args.kernel)
# system.workload = StubWorkload(entry=BASE_ADDR_BOOTROM)
# # = FSWorkload.init_compatible(args.binary)
system.workload = RiscvBareMetal()
zsbl = generate_bootrom(system)
# zsbl = "/nfs/home/pravenel/cva6master/corev_apu/bootrom/bootrom.elf"
system.workload.bootloader = zsbl

# system.workload.reset_vect = 42

# print(system.workload)

# system.workload = RiscvLinux()
# system.workload.addr_check = False
# sysctem.workload.object_file = bbl_path
# system.workload.dtb_filename = path.join(m5.options.outdir, 'device.dtb')
# system.workload.dtb_addr = 0x87e00000
# kernel_cmd = [
#     # specifying Linux kernel boot options
#     "console=ttyS0"
# ]
# system.workload.command_line = " ".join(kernel_cmd)

for cpu in system.cpu:
    cpu.createInterruptController()

root = Root(full_system=True, system=system)

from common import Simulation

# from common import ObjectList
# cls = ObjectList.cpu_list.get('RiscvTimingSimpleCPU')
# ObjectList.cpu_list.print()
# x = cls, cls.memory_mode()
# print(cls)


def simulate_with_checkpoint(totaltime=None):
    start_time = m5.curTick()
    while totaltime is None or (m5.curTick() - start_time) < totaltime:
        if totaltime is None:
            exit_event = m5.simulate()
        else:
            exit_event = m5.simulate(start_time + totaltime - m5.curTick())
        exit_cause = exit_event.getCause()
        cur_tick = m5.curTick()
        print(f"Exiting @ tick {cur_tick} because {exit_cause}")
        if exit_cause == "checkpoint":
            cpt_path = path.join(m5.options.outdir, f"cpt.{cur_tick}")
            print(f"*** Take checkpoint : {cpt_path}")
            m5.checkpoint(cpt_path)
        elif exit_cause != 0:
            print("Simulated exit code not 0! Exit code is", exit_cause)
            break


if args.simpoint_profile:
    if not CONFIG_USE_ATOMIC:
        # Sanity check
        fatal("SimPoint/BPProbe should be done with an atomic cpu")
    print(f"*** simpoint : Setup profile : {args.simpoint_interval}")
    system.cpu[0].addSimPointProbe(args.simpoint_interval)

if args.maxinsts:
    print(f"*** config : Setup maxinsts : {args.maxinsts}")
    system.cpu[0].max_insts_any_thread = args.maxinsts


# Parse simpoints
simpoints = []
if args.take_simpoint_checkpoints != None:
    (
        simpoint_filename,
        weight_filename,
        INTERVAL,
        WARMUP,
    ) = args.take_simpoint_checkpoints.split(",", 3)
    print("simpoint analysis file:", simpoint_filename)
    print("simpoint weight file:", weight_filename)
    print("interval length:", INTERVAL)
    print("warmup length:", WARMUP)
    INTERVAL = int(INTERVAL)
    WARMUP = int(WARMUP)
    with open(simpoint_filename) as simpoint_stream:
        with open(weight_filename) as weight_stream:
            for ls, lw in zip(simpoint_stream, weight_stream):
                ts = int(ls.split(" ")[0])
                we = float(lw.split(" ")[0])
                if ts * INTERVAL - WARMUP > 0:
                    start = ts * INTERVAL - WARMUP
                    warm = WARMUP
                else:
                    # Not enough room for proper warmup
                    # Just starting from the beginning
                    start = 0
                    warm = ts * INTERVAL
                simpoints.append((start, warm, we))

    simpoints.sort(key=lambda x: x[0])
    starts = list(map(lambda x: x[0], simpoints))
    print(starts)
    system.cpu[0].simpoint_start_insts = starts
    for s in simpoints:
        print(
            f"Simpoint: @{s[0]}: [{s[0]+s[1]}-{s[1]}",
            f" {s[0] + s[1] + INTERVAL}] x {s[2]}",
        )
    print(f"Coverage : {sum(map(lambda x: x[2], simpoints))}")

if args.simcpt:
    print(f"*** simpoint restore : {args.simcpt}")
    spl = args.simcpt.split("_")
    assert spl[-2] == "warmup"
    assert spl[-4] == "interval"
    interval = int(spl[-3])
    warm = int(spl[-1])
    print(f"*** simpoint restore {interval}, {warm}")
    system.cpu[0].simpoint_start_insts = [warm, warm + interval]

    m5.instantiate(args.simcpt)
    print(f"*** simpoint restore @{m5.curTick()}, {[warm, warm + interval]}")

    exit_event = m5.simulate()
    exit_cause = exit_event.getCause()
    if exit_cause == "simpoint starting point found":
        print(f"*** simpoint restore @{m5.curTick()}")
        m5.stats.dump()
        m5.stats.reset()

        exit_event = m5.simulate()
        exit_cause = exit_event.getCause()

        if exit_cause == "simpoint starting point found":
            print("Done running SimPoint!")
            sys.exit(exit_event.getCode())

    print("Exiting @ tick %i because %s" % (m5.curTick(), exit_cause))
    sys.exit(exit_event.getCode())


if args.cpt:
    print(f"*** Restore checkpoint {args.cpt}")
    m5.instantiate(args.cpt)
    m5.stats.reset()
else:
    m5.instantiate()


if args.take_simpoint_checkpoints != None:
    last_chkpnt_inst_count = -1
    for index, (ts, warm, weight) in enumerate(simpoints):
        if ts == last_chkpnt_inst_count:
            # checkpoint starting point same as last time
            # (when warmup period longer than starting point)
            exit_cause = "simpoint starting point found"
            code = 0
        else:
            exit_event = m5.simulate()

            # skip checkpoint instructions should they exist
            while exit_event.getCause() == "checkpoint":
                print("Found 'checkpoint' exit event...ignoring...")
                exit_event = m5.simulate()

            exit_cause = exit_event.getCause()
            code = exit_event.getCode()

        if exit_cause == "simpoint starting point found":
            m5.checkpoint(
                path.join(
                    m5.options.outdir,
                    "cpt.simpoint_%02d_inst_%d_weight_%f_interval_%d_warmup_%d"
                    % (index, ts, weight, INTERVAL, warm),
                )
            )
            print(
                "Checkpoint #%d written. start inst:%d weight:%f"
                % (index, ts, weight)
            )
            last_chkpnt_inst_count = ts
        else:
            break
        index += 1

    print("Exiting @ tick %i because %s" % (m5.curTick(), exit_cause))
    sys.exit(code)

else:
    while True:
        exit_event = m5.simulate()
        exit_cause = exit_event.getCause()
        cur_tick = m5.curTick()
        print(f"Exiting @ tick {cur_tick} because {exit_cause}")
        if exit_cause == "checkpoint":
            cpt_path = path.join(m5.options.outdir, f"cpt.{cur_tick}")
            print(f"*** Take checkpoint : {cpt_path}")
            m5.checkpoint(cpt_path)
            if args.exitOnCpt:
                print(f"*** checkpoint exit requested")
                sys.exit(0)

        elif exit_cause != 0:
            print("Simulated exit code not 0! Exit code is", exit_cause)
            break
# else:
#     Simulation.run(args, root, system, None)
