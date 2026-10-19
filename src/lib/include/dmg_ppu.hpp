#ifndef LOTUSGB_DMG_PPU_HPP
#define LOTUSGB_DMG_PPU_HPP

#include <array>
#include <cstdint>

#include "IInterruptController.hpp"
#include "address_bus.hpp"
#include "system_clock.hpp"

namespace LOTUSGB {

// Initial DMG PPU timing and memory interface. Rendering is implemented in a
// later step; mode 3 currently uses its minimum duration without penalties.
class DmgPpu final : public IAddressRegion, public IClockedComponent {
public:
    explicit DmgPpu(IInterruptController &interrupts);

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void advanceTCycles(uint64_t tCycles) override;

private:
    uint8_t getMode() const;
    bool lcdEnabled() const;
    bool lyEqualsLyc() const;
    bool statInterruptSignal() const;
    void updateStatInterruptLine();
    void advanceDot();

    IInterruptController &interrupts;
    std::array<uint8_t, 0x2000> videoRam{};
    std::array<uint8_t, 0x00A0> objectRam{};

    uint8_t lcdControl = 0;
    uint8_t statInterruptEnable = 0;
    uint8_t scrollY = 0;
    uint8_t scrollX = 0;
    uint8_t ly = 0;
    uint8_t lyCompare = 0;
    uint8_t bgPalette = 0;
    uint8_t objectPalette0 = 0;
    uint8_t objectPalette1 = 0;
    uint8_t windowY = 0;
    uint8_t windowX = 0;
    uint16_t dot = 0;
    bool statInterruptLine = false;
};

}

#endif // LOTUSGB_DMG_PPU_HPP
