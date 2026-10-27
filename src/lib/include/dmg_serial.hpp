#ifndef LOTUSGB_DMG_SERIAL_HPP
#define LOTUSGB_DMG_SERIAL_HPP

#include <cstdint>

#include "IInterruptController.hpp"
#include "address_bus.hpp"
#include "system_clock.hpp"

namespace LOTUSGB {

// DMG serial port with no connected peer. Its free-running internal clock is
// independent of DIV. External-clock transfers wait for a peer indefinitely.
class DmgSerial final : public IAddressRegion, public IClockedComponent {
public:
    explicit DmgSerial(IInterruptController &interrupts);
    void initializeDmgPostBootState();
    void initializeDmgBootRomState();
    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void advanceTCycles(uint64_t tCycles) override;

private:
    void clockBit();

    IInterruptController &interrupts;
    uint8_t data = 0;
    uint8_t control = 0;
    unsigned bitsTransferred = 0;
    unsigned clockPhaseTCycles = 0;
};

}

#endif // LOTUSGB_DMG_SERIAL_HPP
