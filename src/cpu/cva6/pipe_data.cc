/**
 * @file pipe_data.hh
 * @author Pierre Ravenel (pravenel@kalrayinc.com)
 * @brief
 * @version 1.0
 * @date 2023-05-25
 */

#include "cpu/cva6/pipe_data.hh"

namespace gem5 {
namespace cva6 {

BranchData*
BranchData::bloup = [](){ return new BranchData(); }();

std::ostream&
BranchData::dump(std::ostream &os) const {
    os << "BranchData(";
    if (isBubble()) {
        os << "bubble";
    } else {
        if (is_predicted){
                os << "prediction ";
            if (need_squash){
                os << "KO";
            } else {
                os << "OK";
            }
        }
        os << ";num=" << num;
        os << ";0x" << std::hex << target->instAddr() << std::dec;
        os << ';';
        if (need_squash){
            os << " [squash:0x" << std::hex
               << squash_target->instAddr() << std::dec
               << "]";
        }
    }
    os << ")";
    return os;
}


std::ostream&
ForwardLineData::dump(std::ostream &os) const {
    os << "ForwardLineData(";
    if (fault != NoFault) {
        os << "fault: " << fault->name();
    } else {
        os << "lineBaseAddr=" << std::hex << lineBaseAddr << ", ";
        os << "pc=" << pc.get()->instAddr() << ", ";
        os << "fetchAddr=" << fetchAddr << ", ";
        os << "lineWidth=" << lineWidth << ", ";
        assert(line);
        if (line) {
            os << "line=[";
            for (int i = 0; i < lineWidth; i++){
                os << std::hex << (unsigned int)line[i];
            }
            os << "]";
        }
    }
    os << ")";
    return os;
}


std::ostream&
ForwardLineDataReg::dump(std::ostream &os) const {
    os << "ForwardLineDataReg(";
    for (auto *x: fifo){
        os << *x << ",";
    }
    os << ')';
    return os;
}


std::ostream&
ForwardInstData::dump(std::ostream &os) const {
    os << "ForwardInstData(";
    for (Cva6DynInstPtr inst: insts){
        os << *inst << ",";
    }
    os << ')';
    return os;
}


} // namespace cva6
} // namespace gem5
