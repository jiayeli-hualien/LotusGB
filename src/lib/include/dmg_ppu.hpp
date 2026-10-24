#ifndef LOTUSGB_DMG_PPU_HPP
#define LOTUSGB_DMG_PPU_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "IInterruptController.hpp"
#include "address_bus.hpp"
#include "system_clock.hpp"

namespace LOTUSGB {

// Initial DMG PPU timing, memory interface, and scanline renderer. Pixels are
// emitted during mode 3, with SCX fine-scroll, visible-window, and object-fetch
// stalls included.
class DmgPpu final : public IAddressRegion, public IClockedComponent {
public:
    static constexpr size_t FRAME_WIDTH = 160;
    static constexpr size_t FRAME_HEIGHT = 144;
    static constexpr size_t OAM_SIZE = 0x00A0;
    using FrameBuffer = std::array<uint8_t, FRAME_WIDTH * FRAME_HEIGHT>;

    explicit DmgPpu(IInterruptController &interrupts);

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void advanceTCycles(uint64_t tCycles) override;
    void initializeDmgPostBootState();
    void writeOamFromDma(size_t offset, uint8_t value);

    // Each pixel is a DMG shade index (0 is lightest, 3 is darkest), after
    // applying BGP/OBP0/OBP1. The frontend chooses the actual output colors.
    const FrameBuffer &getFrameBuffer() const;

private:
    uint8_t getMode() const;
    uint8_t reportedLy() const;
    bool lcdEnabled() const;
    bool lyEqualsLyc() const;
    bool statInterruptSignal() const;
    void updateStatInterruptLine();
    void advanceDot();
    void prepareScanline();
    void renderPixel(size_t screenX);
    void advanceBackgroundFetch();
    uint8_t readBackgroundBitplane(bool high) const;
    void prepareObjects();
    uint8_t getTilePixel(uint8_t tileIndex, uint8_t x, uint8_t y,
                         bool unsignedTileData) const;
    uint8_t getTileMapPixel(uint16_t tileMapOffset, uint8_t x, uint8_t y) const;
    static uint8_t applyPalette(uint8_t palette, uint8_t colorIndex);

    IInterruptController &interrupts;
    std::array<uint8_t, 0x2000> videoRam{};
    std::array<uint8_t, OAM_SIZE> objectRam{};
    FrameBuffer frameBuffer{};
    std::array<uint8_t, FRAME_WIDTH> scanlineBgColorIndices{};
    std::array<uint8_t, FRAME_WIDTH> scanlineObjectColorIndices{};
    std::array<bool, FRAME_WIDTH> scanlineObjectUsesPalette1{};
    std::array<bool, FRAME_WIDTH> scanlineObjectBehindBg{};
    std::array<uint16_t, FRAME_WIDTH> scanlineObjectPenaltyAtX{};

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
    bool windowYTriggered = false;
    uint16_t dot = 0;
    uint16_t mode3Duration = 172;
    uint8_t scanlineFineScroll = 0;
    uint8_t backgroundFetchTile = 0;
    uint8_t backgroundFetchLow = 0;
    uint8_t backgroundFetchHigh = 0;
    uint16_t backgroundFetchTicks = 0;
    uint8_t scanlineOutputX = 0;
    uint8_t scanlineWindowStartX = 0;
    uint8_t windowPenaltyDotsRemaining = 0;
    uint16_t objectPenaltyDotsRemaining = 0;
    bool scanlineWindowPenaltyPending = false;
    bool scanlineDrewWindow = false;
    bool startupScanline = false;
    bool lycCoincidence = true;
    bool statInterruptLine = false;
};

}

#endif // LOTUSGB_DMG_PPU_HPP
