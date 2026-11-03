#ifndef LOTUSGB_IMEMORY_ACCESS_HPP
#define LOTUSGB_IMEMORY_ACCESS_HPP

#include <cstdint>

namespace LOTUSGB {

enum class CpuBusEvent { Read, Write, ReadIdu, WriteIdu, Idu };

class IMemoryAccess {
    // CPU read/write calls occur at the bus phase within the M-cycle advanced
    // by ICPU::stepOneCycle(). Implementations may synchronize devices to that
    // point before completing the transaction.
public:
    virtual ~IMemoryAccess() = default;
    virtual uint8_t read(uint16_t addr) = 0;
    virtual void write(uint16_t addr, uint8_t value) = 0;
    // Flat-memory/GBIT backends have no IDU bus side effects; default to their
    // ordinary transaction. Hardware buses override these combined M-cycle events.
    virtual uint8_t readWithIdu(uint16_t addr) { return read(addr); }
    virtual void writeWithIdu(uint16_t addr, uint8_t value) { write(addr, value); }
    virtual void notifyIdu(uint16_t) {} // Internal cycle, no data read/write.
    virtual void resize(size_t size) = 0;
};

}
#endif // LOTUSGB_IMEMORY_HPP
