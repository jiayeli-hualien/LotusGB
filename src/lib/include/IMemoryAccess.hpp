#ifndef LOTUSGB_IMEMORY_ACCESS_HPP
#define LOTUSGB_IMEMORY_ACCESS_HPP

#include <cstdint>

namespace LOTUSGB {

class IMemoryAccess {
    // CPU read/write calls occur synchronously inside the M-cycle being
    // advanced by ICPU::stepOneCycle(). A system bus can therefore observe
    // each transaction at M-cycle granularity; T-cycle ordering is deferred.
public:
    virtual ~IMemoryAccess() = default;
    virtual uint8_t read(uint16_t addr) = 0;
    virtual void write(uint16_t addr, uint8_t value) = 0;
    virtual void resize(size_t size) = 0;
};

}
#endif // LOTUSGB_IMEMORY_HPP
