#ifndef LOTUSGB_TIMER_HPP
#define LOTUSGB_TIMER_HPP

#include <cstdint>

#include "IInterruptController.hpp"
#include "address_bus.hpp"
#include "system_clock.hpp"

namespace LOTUSGB {

// DMG timer registers and divider. The timer input is derived from a falling
// edge of one bit of the internal 16-bit system counter.
class Timer final : public IAddressRegion, public IClockedComponent {
public:
    explicit Timer(IInterruptController &interrupts);

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void advanceTCycles(uint64_t tCycles) override;
    void initializeDmgPostBootState();

private:
    bool timerInput() const;
    void incrementTima();
    void applyTimerInputTransition(bool oldInput);

    IInterruptController &interrupts;
    uint16_t systemCounter = 0;
    uint8_t tima = 0;
    uint8_t tma = 0;
    uint8_t tac = 0;
    int overflowReloadTCycles = 0;
};

}

#endif // LOTUSGB_TIMER_HPP
