#ifndef LOTUSGB_DMG_PPU_HPP
#define LOTUSGB_DMG_PPU_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "IInterruptController.hpp"
#include "address_bus.hpp"
#include "system_clock.hpp"

namespace LOTUSGB {

// Initial DMG PPU timing, memory interface, and scanline renderer. Mode 3
// currently uses its minimum duration without rendering penalties.
class DmgPpu final : public IAddressRegion, public IClockedComponent {
public:
    static constexpr size_t FRAME_WIDTH = 160;
    static constexpr size_t FRAME_HEIGHT = 144;
    using FrameBuffer = std::array<uint8_t, FRAME_WIDTH * FRAME_HEIGHT>;

    explicit DmgPpu(IInterruptController &interrupts);

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void advanceTCycles(uint64_t tCycles) override;
    void initializeDmgPostBootState();

    // Each pixel is a DMG shade index (0 is lightest, 3 is darkest), after
    // applying BGP/OBP0/OBP1. The frontend chooses the actual output colors.
    const FrameBuffer &getFrameBuffer() const;

private:
    uint8_t getMode() const;
    bool lcdEnabled() const;
    bool lyEqualsLyc() const;
    bool statInterruptSignal() const;
    void updateStatInterruptLine();
    void advanceDot();
    void renderScanline();
    void renderObjects(const std::array<uint8_t, FRAME_WIDTH> &bgColorIndices,
                       size_t lineOffset);
    uint8_t getTilePixel(uint8_t tileIndex, uint8_t x, uint8_t y,
                         bool unsignedTileData) const;
    uint8_t getTileMapPixel(uint16_t tileMapOffset, uint8_t x, uint8_t y) const;
    static uint8_t applyPalette(uint8_t palette, uint8_t colorIndex);

    IInterruptController &interrupts;
    std::array<uint8_t, 0x2000> videoRam{};
    std::array<uint8_t, 0x00A0> objectRam{};
    FrameBuffer frameBuffer{};

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
    uint8_t windowLineCounter = 0;
    uint16_t dot = 0;
    bool statInterruptLine = false;
};

}

#endif // LOTUSGB_DMG_PPU_HPP
