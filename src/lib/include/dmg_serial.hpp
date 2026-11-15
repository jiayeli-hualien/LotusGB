#ifndef LOTUSGB_DMG_SERIAL_HPP
#define LOTUSGB_DMG_SERIAL_HPP

#include <cstdint>
#include <functional>

#include "IInterruptController.hpp"
#include "address_bus.hpp"
#include "system_clock.hpp"

namespace LOTUSGB {

// Shared DMG/CGB serial port. Internal clocks are independent of DIV; an
// unconnected internal transfer reads ones, while external mode awaits edges.
class DmgSerial final : public IAddressRegion, public IClockedComponent {
    friend class StateCodec;
public:
    explicit DmgSerial(IInterruptController &interrupts, bool cgbMode = false);
    void setCgbFeaturesEnabled(bool enabled) { cgbMode = enabled; }
    void initializeDmgPostBootState();
    void initializeDmgBootRomState();
    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void advanceTCycles(uint64_t tCycles) override;
    // Logical next outgoing bit (SB bit 7), not an electrical line model.
    bool getOutgoingBit() const;
    unsigned getBitsTransferred() const;
    // One externally supplied sampling edge; ignored unless external transfer is armed.
    void clockExternalBit(bool incomingBit);
    // Called once per internal sampling edge with outgoing bit; returns incoming bit.
    // Empty restores the disconnected incoming-high behavior. Host wiring is not state.
    void setInternalClockCallback(std::function<bool(bool)> callback);

private:
    void clockBit();
    void shiftBit(bool incomingBit);
    std::function<bool(bool)> internalClockCallback;

    IInterruptController &interrupts;
    bool cgbMode;
    uint8_t data = 0;
    uint8_t control = 0;
    unsigned bitsTransferred = 0;
    unsigned clockPhaseTCycles = 0;
};

}

#endif // LOTUSGB_DMG_SERIAL_HPP
