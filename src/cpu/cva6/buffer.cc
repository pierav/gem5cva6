#include <iomanip>

#include "cpu/cva6/buffers.hh"

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

}
}
