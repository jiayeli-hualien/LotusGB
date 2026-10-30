#ifndef LOTUSGB_INTERRUPT_REGISTERS_HPP
#define LOTUSGB_INTERRUPT_REGISTERS_HPP

#include <cstdint>

#include "IInterruptController.hpp"
#include "address_bus.hpp"

namespace LOTUSGB {

class InterruptRegisters final : public IAddressRegion, public IInterruptController {
    friend class StateCodec;
public:
    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;

    void requestTimerInterrupt();
    uint8_t getPendingInterrupts() const override;
    void acknowledgeInterrupt(uint8_t interruptMask) override;
    void requestInterrupt(uint8_t interruptMask) override;
    uint8_t getRequestedInterrupts() const { return getPendingInterrupts(); }

private:
    uint8_t interruptFlags = 0;
    uint8_t interruptEnable = 0;
};

}

#endif // LOTUSGB_INTERRUPT_REGISTERS_HPP
