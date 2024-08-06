#include "cpu/cva6/registerfile.hh"

namespace gem5 {
namespace cva6 {


const std::vector<std::string> FloatRegNames = {
    "ft0", "ft1", "ft2", "ft3",
    "ft4", "ft5", "ft6", "ft7",
    "fs0", "fs1", "fa0", "fa1",
    "fa2", "fa3", "fa4", "fa5",
    "fa6", "fa7", "fs2", "fs3",
    "fs4", "fs5", "fs6", "fs7",
    "fs8", "fs9", "fs10", "fs11",
    "ft8", "ft9", "ft10", "ft11"
};

const std::vector<std::string> IntRegNames = {
    "zero", "ra", "sp", "gp",
    "tp", "t0", "t1", "t2",
    "s0", "s1", "a0", "a1",
    "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3",
    "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11",
    "t3", "t4", "t5", "t6"
};

std::string riscvRegisterName(RegId reg){
  return RiscvISA::registerName(reg);
}

bool reverseRegisterName(Cva6CPU &cpu, char* name, RegId &reg){
  int index = 0;
  RegClassType type = IntRegClass;
  for (int i = 0; i < IntRegNames.size(); i++){
    if (strcmp(IntRegNames[i].c_str(), name) == 0){
      type = IntRegClass;
      index = i;
      goto done;
    }
  }
  for (int i = 0; i < FloatRegNames.size(); i++){
    if (strcmp(FloatRegNames[i].c_str(), name) == 0){
      type = FloatRegClass;
      index = i;
      goto done;
    }
  }
  return false;
  // fatal("Invalid reg translation : %s", name);
  /* Todo some kind of x* ? */
  done:
  BaseISA::RegClasses rcs = cpu.thread->getIsaPtr()->regClasses();
  // const RegClass *rc = rcs.at(type);
  reg = RegId(*rcs.at(type), index);
  /* Final check*/
  fatal_if(strcmp(riscvRegisterName(reg).c_str(), name) != 0,
    "Invalid reg translation : %s", name);
  return true;
}

std::string
BinaryRegisterFile::dump(const uint64_t *vals){
  std::stringstream ss;
  ss << "{ ";
  int cnt = 0;
  for (int i = 0; i < 64; i++){
      if (bitset & ((uint64_t)1 << i)){
          ss <<  "\033[34m";
          if (i<32){
              ss << IntRegNames[i];
          } else {
              ss << FloatRegNames[i-32];
          }
          ss << "\033[39m";
          if (vals){
              ss << "=" << vals[cnt];
          }
          cnt ++;
          if (bitset >> i != 1){
              ss << ", ";
          }
      }
  }
  ss << '}';
  return ss.str();
}


} // namespace cva6
} // namespace gem5
