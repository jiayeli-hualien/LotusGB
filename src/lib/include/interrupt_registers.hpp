#ifndef LOTUSGB_INTERRUPT_REGISTERS_HPP
#define LOTUSGB_INTERRUPT_REGISTERS_HPP

#include <cstdint>

#include "address_bus.hpp"

namespace LOTUSGB {

class InterruptRegisters final : public IAddressRegion {
public:
    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;

    void requestTimerInterrupt();
    uint8_t getRequestedInterrupts() const;

private:
    uint8_t interruptFlags = 0;
    uint8_t interruptEnable = 0;
};

}

#endif // LOTUSGB_INTERRUPT_REGISTERS_HPP
