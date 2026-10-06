
# CVA6 in gem5.

This project provides a performance model of the OpenHW CVA6 processor implemented in gem5.

The CPU implementation is located in `src/cpu/cva6`, and the corresponding configuration script is `configs/example/riscv_cva6.py`.

```sh
# build
scons build/RISCV/gem5.opt -j 32
# Demo binary
riscv64-unknown-elf-gcc -x assembler-with-cpp - -nostdlib -nostartfiles -march=rv64imafdc -mabi=lp64d -Ttext=0x80000000 -e _start -Wl,-n -o x.elf << 'EOF'
.text
_start:
.globl _start
li t0, 42
pass:
.globl pass
j pass
fail:
.globl fail
j fail
EOF
# Run Gem5
./build/RISCV/gem5.opt --debug-flags=Cva6Commit ./configs/example/riscv_cva6.py --kernel=x.elf
# src/sim/simulate.cc:199: info: Entering event queue @ 0.  Starting simulation...
#   60000: global: commit: [M] 0x10000 sn:1: addi s0, zero, 1               s0:%0%%%:0000000000000001
#   61000: global: commit: [M] 0x10004 sn:2: slli s0, s0, 31                s0:%1%%%:0000000080000000 *s0:%0%%%:0000000000000001
#   61000: global: commit: [M] 0x10008 sn:3: csrrs a0, mhartid, zero        a0:%2%%%:0000000000000000
#  120000: global: commit: [M] 0x1000c sn:27: auipc a1, 0                    a1:%0%%%:000000000001000c
#  121000: global: commit: [M] 0x10010 sn:28: addi a1, a1, 116               a1:%1%%%:0000000000010080 *a1:%0%%%:000000000001000c
#  121000: global: commit: [M] 0x10014 sn:29: jalr zero, 0(s0)               s0:A%0%%%:0000000080000000 [T:1 =>80000000 MISS (T:0 =>10018)]
#  178000: global: commit: [M] 0x80000000 sn:50: addi t0, zero, 42              t0:%0%%%:000000000000002a
#  178000: global: commit: [M] 0x80000004 sn:51: c_j 0                         [T:1 =>80000004 HIT]
# *** GOOD TRAP *** @0x80000004
# Exiting @ tick 178000 because *** GOOD TRAP ***
```

# Publication

The work available in this repository was published at the 2023 RISC-V Summit Europe under the title:

> A gem5-based CVA6 Framework for Microarchitectural Pathfinding

```bibtex
@inproceedings{ravenel2023gem5,
  title={A gem5-based CVA6 Framework for Microarchitectural Pathfinding},
  author={Ravenel, Pierre and Perais, Arthur and De Dinechin, Beno{\^\i}t and P{\'e}trot, Fr{\'e}d{\'e}ric},
  booktitle={RISC-V Summit Eur},
  year={2023}
}
```

# Notes

The CPU model implements the following improvements by default:

* TAGE branch predictor
* 4-way issue from fetch to commit
* Larger LSU, ROB, register files, and related structures
* Store-to-load forwarding (STLF)
* Optional out-of-order execution with --sbOoO

There are also a few important differences between the CVA6 RTL and this gem5 model:

* The CVA6 RTL does not implement a decoupled fetch, whereas gem5 does. The BTB size therefore needs to be adjusted to maintain a realistic configuration.
* gem5 does not currently provide a write-through (WT) cache model, which makes tuning the model to accurately match the RTL more challenging.




# The gem5 Simulator

This is the repository for the gem5 simulator. It contains the full source code
for the simulator and all tests and regressions.

The gem5 simulator is a modular platform for computer-system architecture
research, encompassing system-level architecture as well as processor
microarchitecture. It is primarily used to evaluate new hardware designs,
system software changes, and compile-time and run-time system optimizations.

The main website can be found at <http://www.gem5.org>.

## Testing status

**Note**: These regard tests run on the develop branch of gem5:
<https://github.com/gem5/gem5/tree/develop>.

[![Daily Tests](https://github.com/gem5/gem5/actions/workflows/daily-tests.yaml/badge.svg)](https://github.com/gem5/gem5/actions/workflows/daily-tests.yaml)
[![Weekly Tests](https://github.com/gem5/gem5/actions/workflows/weekly-tests.yaml/badge.svg)](https://github.com/gem5/gem5/actions/workflows/weekly-tests.yaml)
[![Compiler Tests](https://github.com/gem5/gem5/actions/workflows/compiler-tests.yaml/badge.svg)](https://github.com/gem5/gem5/actions/workflows/compiler-tests.yaml)

## Getting started

A good starting point is <http://www.gem5.org/about>, and for
more information about building the simulator and getting started
please see <http://www.gem5.org/documentation> and
<http://www.gem5.org/documentation/learning_gem5/introduction>.

## Building gem5

To build gem5, you will need the following software: g++ or clang,
Python (gem5 links in the Python interpreter), SCons, zlib, m4, and lastly
protobuf if you want trace capture and playback support. Please see
<http://www.gem5.org/documentation/general_docs/building> for more details
concerning the minimum versions of these tools.

Once you have all dependencies resolved, execute
`scons build/ALL/gem5.opt` to build an optimized version of the gem5 binary
(`gem5.opt`) containing all gem5 ISAs. If you only wish to compile gem5 to
include a single ISA, you can replace `ALL` with the name of the ISA. Valid
options include `ARM`, `NULL`, `MIPS`, `POWER`, `RISCV`, `SPARC`, and `X86`
The complete list of options can be found in the build_opts directory.

See https://www.gem5.org/documentation/general_docs/building for more
information on building gem5.

## The Source Tree

The main source tree includes these subdirectories:

* build_opts: pre-made default configurations for gem5
* build_tools: tools used internally by gem5's build process.
* configs: example simulation configuration scripts
* ext: less-common external packages needed to build gem5
* include: include files for use in other programs
* site_scons: modular components of the build system
* src: source code of the gem5 simulator. The C++ source, Python wrappers, and Python standard library are found in this directory.
* system: source for some optional system software for simulated systems
* tests: regression tests
* util: useful utility programs and files

## gem5 Resources

To run full-system simulations, you may need compiled system firmware, kernel
binaries and one or more disk images, depending on gem5's configuration and
what type of workload you're trying to run. Many of these resources can be
obtained from <https://resources.gem5.org>.

More information on gem5 Resources can be found at
<https://www.gem5.org/documentation/general_docs/gem5_resources/>.

## Getting Help, Reporting bugs, and Requesting Features

We provide a variety of channels for users and developers to get help, report
bugs, requests features, or engage in community discussions. Below
are a few of the most common we recommend using.

* **GitHub Discussions**: A GitHub Discussions page. This can be used to start
discussions or ask questions. Available at
<https://github.com/orgs/gem5/discussions>.
* **GitHub Issues**: A GitHub Issues page for reporting bugs or requesting
features. Available at <https://github.com/gem5/gem5/issues>.
* **Jira Issue Tracker**: A Jira Issue Tracker for reporting bugs or requesting
features. Available at <https://gem5.atlassian.net/>.
* **Slack**: A Slack server with a variety of channels for the gem5 community
to engage in a variety of discussions. Please visit
<https://www.gem5.org/join-slack> to join.
* **gem5-users@gem5.org**: A mailing list for users of gem5 to ask questions
or start discussions. To join the mailing list please visit
<https://www.gem5.org/mailing_lists>.
* **gem5-dev@gem5.org**: A mailing list for developers of gem5 to ask questions
or start discussions. To join the mailing list please visit
<https://www.gem5.org/mailing_lists>.

## Contributing to gem5

We hope you enjoy using gem5. When appropriate we advise charing your
contributions to the project. <https://www.gem5.org/contributing> can help you
get started. Additional information can be found in the CONTRIBUTING.md file.
