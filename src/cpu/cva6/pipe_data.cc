#include "cpu/cva6/pipe_data.hh"

namespace gem5 {
namespace cva6 {

BranchData*
BranchData::bloup = [](){ return new BranchData(); }();

std::ostream &
operator <<(std::ostream &os, const BranchData &branch){
    os << branch.dump();
    return os;
}

void
ForwardLineData::setFault(Fault fault_)
{
    fault = fault_;
    if (isFault())
        bubbleFlag = false;
}

void
ForwardLineData::allocateLine(unsigned int width_)
{
    lineWidth = width_;
    bubbleFlag = false;

    assert(!isFault());
    assert(!line);

    line = new uint8_t[width_];
}

void
ForwardLineData::adoptPacketData(Packet *packet)
{
    this->packet = packet;
    lineWidth = packet->req->getSize();
    bubbleFlag = false;

    assert(!isFault());
    assert(!line);

    line = packet->getPtr<uint8_t>();
}

void
ForwardLineData::freeLine()
{
    /* Only free lines in non-faulting, non-bubble lines */
    if (!isFault() && !isBubble()) {
        assert(line);
        /* If packet is not NULL then the line must belong to the packet so
         *  we don't need to separately deallocate the line */
        if (packet) {
            delete packet;
        } else {
            delete [] line;
        }
        line = NULL;
        bubbleFlag = true;
    }
}


void
ForwardLineData::reportData(std::ostream &os) const
{
    os << "ForwardLineData(";
    if (bubbleFlag){
        os << "bubble";
    } else if (fault != NoFault) {
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
}


std::string
ForwardInstData::dump() {
    std::ostringstream os;
    reportData(os);
    return os.str();
}


void
ForwardInstData::reportData(std::ostream &os) const {
    os << "ForwardInstData(";
    for (Cva6DynInstPtr inst: insts){
        os << *inst << ",";
    }
    os << ')';
}

} // namespace cva6
} // namespace gem5
