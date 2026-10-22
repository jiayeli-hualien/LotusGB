#ifndef LOTUSGB_IMEMORY_ACCESS_HPP
#define LOTUSGB_IMEMORY_ACCESS_HPP

#include <cstdint>

namespace LOTUSGB {

class IMemoryAccess {
    // CPU read/write calls occur at the bus phase within the M-cycle advanced
    // by ICPU::stepOneCycle(). Implementations may synchronize devices to that
    // point before completing the transaction.
public:
    virtual ~IMemoryAccess() = default;
    virtual uint8_t read(uint16_t addr) = 0;
    virtual void write(uint16_t addr, uint8_t value) = 0;
    virtual void resize(size_t size) = 0;
};

}
#endif // LOTUSGB_IMEMORY_HPP
