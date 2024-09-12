/**
 * memory.hh
 *
 *  Author:   Pierre Ravenel
 * Created:   03/05/2024
 **/

#include <cstring>
#include <map>

namespace gem5 {
namespace cva6 {

class InfiniteMemory64 : Named
{
    struct InfiniteMemory64Stats : public statistics::Group
    {
        statistics::Scalar req;
        statistics::Scalar storeReqs;
        statistics::Scalar storeSilent;
        /* todo (not trivial !) tatistics::Scalar storeToStore; */

        statistics::Scalar loadReqs;
        statistics::Scalar loadConstant;
        statistics::Scalar loadToLoad;

        InfiniteMemory64Stats(Cva6CPU &cpu) :
        statistics::Group(&cpu, "checker"),
        ADD_STAT(req, "Requests"),
        ADD_STAT(storeReqs, "Number of stores"),
        ADD_STAT(storeSilent, "Silent Store"),
        ADD_STAT(loadReqs, "loadReqs"),
        ADD_STAT(loadConstant, "loadConstant"),
        ADD_STAT(loadToLoad, "loadToLoad")
        { }
    }stats;

    struct dwline_t
    {
        uint64_t data = 0;
        bool constant = false;
        bool nostore = false; /* Nostore < Constant */
    };
    std::map<uint64_t, dwline_t> mem;


    inline uint64_t keyof(uint64_t addr){
        return addr >> 3;
    }

    inline uint8_t offset(uint64_t addr){
        return addr & 0b111;
    }

    inline dwline_t* entry(uint64_t addr){
        return &mem[keyof(addr)];
    }
    inline void* memPtr(uint64_t addr){
        return ((uint8_t*)&(entry(addr)->data)) + offset(addr);
    }


    uint64_t _read(uint64_t addr, uint8_t size){
        fatal_if(size > 8, "Invalid size : %d\n", size);
        uint64_t value = 0;
        memcpy(&value, memPtr(addr), size);
        return value;
    }

    public:
    InfiniteMemory64(const std::string &name_, Cva6CPU &cpu_)
        : Named(name_), stats(cpu_) {}

    /**
     * @brief Performs a memory write. Returns true if the new value is equal
     * to the last one.
     *
     * @param addr
     * @param size
     * @param value
     * @return true
     * @return false
     */
    bool check_store(uint64_t addr, uint8_t size, uint64_t value){
        bool res = false; /* Default is no indempotance */
        // printf("W @%16lx #%d : %16lx\n", addr, size, value);
        fatal_if(size > 8, "Invalid size : %d\n", size);
        if (mem.count(keyof(addr)) || (size == 8)){
            uint64_t oldvalue = _read(addr, size);
            entry(addr)->nostore = false; /* Mark store */
            if (oldvalue != value){
                memcpy(memPtr(addr), &value, size);
                entry(addr)->constant = false; /* Mark update */
            } else {
                res = true;
            }
        }
        /* Statistics */
        stats.storeReqs +=1;
        stats.storeSilent += res;
        return res;
    }

    /**
     * @brief Check the load value. Returns true if the dw is const
     *
     * @param addr
     * @param size
     * @param value
     * @return true
     * @return false
     */
    bool check_load(uint64_t addr, uint8_t size, uint64_t value){
        bool loadConstant = false; /* Default is not constant */
        bool loadToLoad = false;
        // printf("L @%16lx #%d : %16lx\n", addr, size, value);
        if (!mem.count(keyof(addr))){ // Data is new
            if ((size == 8)){ // Allocate if possible
                memcpy(memPtr(addr), &value, size);
            }
        } else {
            uint64_t res = _read(addr, size);
            fatal_if(res != value, "Bad value: %lx != %lx\n",
                res, value); // Checker
            loadConstant = entry(addr)->constant;
            loadToLoad = entry(addr)->nostore;
            /* Reset flags */
            entry(addr)->constant = true;
            entry(addr)->nostore = true;
        }
        /* Statistics */
        stats.loadReqs += 1;
        stats.loadConstant += loadConstant;
        stats.loadToLoad += loadToLoad;
        return loadConstant;
    }

    void invalidate(uint64_t addr){
        // printf("I @%16lx\n", addr);
        mem.erase(keyof(addr));
    }
};

}
}
