#ifndef LOTUSGB_DMG_SERIAL_HPP
#define LOTUSGB_DMG_SERIAL_HPP

#include <cstdint>
#include <functional>
#include <optional>

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
    uint8_t getData() const;
    unsigned getBitsTransferred() const;
    // One externally supplied sampling edge; ignored unless external transfer is armed.
    void clockExternalBit(bool incomingBit);
    // Netpacket can complete an active byte transfer atomically at a byte barrier.
    // This is separate from the hardware-timed external edge path above.
    bool completeNetworkByte(uint8_t incomingByte);
    // Called once per internal sampling edge with outgoing bit. A missing bit
    // leaves the transfer pending while the rest of the system keeps running.
    // An empty callback restores disconnected incoming-high behavior.
    void setInternalClockCallback(
        std::function<std::optional<bool>(bool)> callback);

private:
    void clockBit();
    void shiftBit(bool incomingBit);
    std::function<std::optional<bool>(bool)> internalClockCallback;

    IInterruptController &interrupts;
    bool cgbMode;
    uint8_t data = 0;
    uint8_t control = 0;
    unsigned bitsTransferred = 0;
    unsigned clockPhaseTCycles = 0;
};

}

#endif // LOTUSGB_DMG_SERIAL_HPP
