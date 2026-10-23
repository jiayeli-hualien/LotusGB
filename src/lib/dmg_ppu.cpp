#include "include/dmg_ppu.hpp"

#include <algorithm>
#include <array>

namespace LOTUSGB {

namespace {
constexpr uint16_t VRAM_START = 0x8000;
constexpr uint16_t VRAM_END = 0x9FFF;
constexpr uint16_t OAM_START = 0xFE00;
constexpr uint16_t OAM_END = 0xFE9F;

constexpr uint16_t LCDC_ADDRESS = 0xFF40;
constexpr uint16_t STAT_ADDRESS = 0xFF41;
constexpr uint16_t SCY_ADDRESS = 0xFF42;
constexpr uint16_t SCX_ADDRESS = 0xFF43;
constexpr uint16_t LY_ADDRESS = 0xFF44;
constexpr uint16_t LYC_ADDRESS = 0xFF45;
constexpr uint16_t BGP_ADDRESS = 0xFF47;
constexpr uint16_t OBP0_ADDRESS = 0xFF48;
constexpr uint16_t OBP1_ADDRESS = 0xFF49;
constexpr uint16_t WY_ADDRESS = 0xFF4A;
constexpr uint16_t WX_ADDRESS = 0xFF4B;

constexpr uint8_t LCD_ENABLE_MASK = 0x80;
constexpr uint8_t WINDOW_TILE_MAP_MASK = 0x40;
constexpr uint8_t WINDOW_ENABLE_MASK = 0x20;
constexpr uint8_t TILE_DATA_SELECT_MASK = 0x10;
constexpr uint8_t BG_TILE_MAP_MASK = 0x08;
constexpr uint8_t OBJECT_SIZE_MASK = 0x04;
constexpr uint8_t OBJECT_ENABLE_MASK = 0x02;
constexpr uint8_t BG_WINDOW_ENABLE_MASK = 0x01;
constexpr uint8_t STAT_LYC_INTERRUPT_MASK = 0x40;
constexpr uint8_t STAT_MODE2_INTERRUPT_MASK = 0x20;
constexpr uint8_t STAT_MODE1_INTERRUPT_MASK = 0x10;
constexpr uint8_t STAT_MODE0_INTERRUPT_MASK = 0x08;
constexpr uint8_t STAT_INTERRUPT_ENABLE_MASK = 0x78;
constexpr uint8_t STAT_COINCIDENCE_MASK = 0x04;

constexpr uint16_t DOTS_PER_SCANLINE = 456;
constexpr uint16_t MODE2_DOTS = 80;
constexpr uint16_t MODE3_DOTS = 172;
constexpr uint8_t WINDOW_PENALTY_DOTS = 6;
constexpr uint8_t VISIBLE_SCANLINES = 144;
constexpr uint8_t SCANLINES_PER_FRAME = 154;
constexpr size_t OBJECT_ENTRY_SIZE = 4;
constexpr size_t OBJECT_COUNT = 40;
constexpr size_t MAX_OBJECTS_PER_SCANLINE = 10;
}

DmgPpu::DmgPpu(IInterruptController &interrupts) : interrupts(interrupts) {}

void DmgPpu::initializeDmgPostBootState() {
    lcdControl = 0x91;
    statInterruptEnable = 0;
    scrollY = 0;
    scrollX = 0;
    ly = 0;
    lyCompare = 0;
    bgPalette = 0xFC;
    // The DMG boot hand-off does not define these object palette registers.
    objectPalette0 = 0;
    objectPalette1 = 0;
    windowY = 0;
    windowX = 0;
    windowLineCounter = 0;
    dot = 0;
    scanlineDrewWindow = false;
    frameBuffer.fill(0);
    statInterruptLine = false;
}

uint8_t DmgPpu::read(uint16_t address) {
    if (address >= VRAM_START && address <= VRAM_END) {
        if (lcdEnabled() && getMode() == 3)
            return 0xFF;
        return videoRam[address - VRAM_START];
    }

    if (address >= OAM_START && address <= OAM_END) {
        const uint8_t mode = getMode();
        if (lcdEnabled() && (mode == 2 || mode == 3))
            return 0xFF;
        return objectRam[address - OAM_START];
    }

    switch (address) {
        case LCDC_ADDRESS:
            return lcdControl;
        case STAT_ADDRESS: {
            const uint8_t coincidence = lyEqualsLyc() ? STAT_COINCIDENCE_MASK : 0;
            return static_cast<uint8_t>(0x80 | statInterruptEnable | coincidence | getMode());
        }
        case SCY_ADDRESS:
            return scrollY;
        case SCX_ADDRESS:
            return scrollX;
        case LY_ADDRESS:
            return ly;
        case LYC_ADDRESS:
            return lyCompare;
        case BGP_ADDRESS:
            return bgPalette;
        case OBP0_ADDRESS:
            return objectPalette0;
        case OBP1_ADDRESS:
            return objectPalette1;
        case WY_ADDRESS:
            return windowY;
        case WX_ADDRESS:
            return windowX;
        default:
            return 0xFF;
    }
}

void DmgPpu::write(uint16_t address, uint8_t value) {
    if (address >= VRAM_START && address <= VRAM_END) {
        if (!lcdEnabled() || getMode() != 3)
            videoRam[address - VRAM_START] = value;
        return;
    }

    if (address >= OAM_START && address <= OAM_END) {
        const uint8_t mode = getMode();
        if (!lcdEnabled() || (mode != 2 && mode != 3))
            objectRam[address - OAM_START] = value;
        return;
    }

    switch (address) {
        case LCDC_ADDRESS: {
            const bool wasEnabled = lcdEnabled();
            lcdControl = value;
            if (wasEnabled != lcdEnabled()) {
                dot = 0;
                ly = 0;
                windowLineCounter = 0;
                mode3Duration = MODE3_DOTS;
                scanlineFineScroll = 0;
                scanlineOutputX = 0;
                scanlineWindowPenaltyPending = false;
                windowPenaltyDotsRemaining = 0;
                objectPenaltyDotsRemaining = 0;
                scanlineDrewWindow = false;
                if (!lcdEnabled())
                    frameBuffer.fill(0);
                updateStatInterruptLine();
            }
            break;
        }
        case STAT_ADDRESS:
            statInterruptEnable = value & STAT_INTERRUPT_ENABLE_MASK;
            updateStatInterruptLine();
            break;
        case SCY_ADDRESS:
            scrollY = value;
            break;
        case SCX_ADDRESS:
            scrollX = value;
            break;
        case LY_ADDRESS:
            // LY is read-only.
            break;
        case LYC_ADDRESS:
            lyCompare = value;
            updateStatInterruptLine();
            break;
        case BGP_ADDRESS:
            bgPalette = value;
            break;
        case OBP0_ADDRESS:
            objectPalette0 = value;
            break;
        case OBP1_ADDRESS:
            objectPalette1 = value;
            break;
        case WY_ADDRESS:
            windowY = value;
            break;
        case WX_ADDRESS:
            windowX = value;
            break;
        default:
            break;
    }
}

void DmgPpu::advanceTCycles(uint64_t tCycles) {
    if (!lcdEnabled())
        return;

    for (uint64_t cycle = 0; cycle < tCycles; ++cycle)
        advanceDot();
}

void DmgPpu::writeOamFromDma(size_t offset, uint8_t value) {
    if (offset < objectRam.size())
        objectRam[offset] = value;
}

const DmgPpu::FrameBuffer &DmgPpu::getFrameBuffer() const {
    return frameBuffer;
}

uint8_t DmgPpu::getMode() const {
    if (!lcdEnabled())
        return 0;
    if (ly >= VISIBLE_SCANLINES)
        return 1;
    if (dot < MODE2_DOTS)
        return 2;
    if (dot < MODE2_DOTS + mode3Duration)
        return 3;
    return 0;
}

bool DmgPpu::lcdEnabled() const {
    return (lcdControl & LCD_ENABLE_MASK) != 0;
}

bool DmgPpu::lyEqualsLyc() const {
    return ly == lyCompare;
}

bool DmgPpu::statInterruptSignal() const {
    if (!lcdEnabled())
        return false;

    const uint8_t mode = getMode();
    return ((mode == 0) && (statInterruptEnable & STAT_MODE0_INTERRUPT_MASK)) ||
           ((mode == 1) && (statInterruptEnable & STAT_MODE1_INTERRUPT_MASK)) ||
           ((mode == 2) && (statInterruptEnable & STAT_MODE2_INTERRUPT_MASK)) ||
           (lyEqualsLyc() && (statInterruptEnable & STAT_LYC_INTERRUPT_MASK));
}

void DmgPpu::updateStatInterruptLine() {
    const bool signal = statInterruptSignal();
    if (signal && !statInterruptLine)
        interrupts.requestInterrupt(INTERRUPT_LCD_STAT);
    statInterruptLine = signal;
}

void DmgPpu::advanceDot() {
    ++dot;
    if (ly < VISIBLE_SCANLINES && dot == MODE2_DOTS)
        prepareScanline();

    // Mode 3 has a 12-dot startup, plus the initial SCX fine-scroll discard,
    // before its visible output. Keep pixels on the PPU clock so palette
    // writes affect only pixels not yet sent to the LCD.
    const uint16_t firstPixelDot =
        MODE2_DOTS + 13 + scanlineFineScroll;
    if (ly < VISIBLE_SCANLINES && dot >= firstPixelDot &&
        dot < MODE2_DOTS + mode3Duration + 1) {
        bool outputStalled = false;
        if (scanlineWindowPenaltyPending &&
            scanlineOutputX == scanlineWindowStartX) {
            scanlineWindowPenaltyPending = false;
            windowPenaltyDotsRemaining = WINDOW_PENALTY_DOTS - 1;
            outputStalled = true;
        } else if (windowPenaltyDotsRemaining > 0) {
            --windowPenaltyDotsRemaining;
            outputStalled = true;
        } else if (objectPenaltyDotsRemaining > 0) {
            --objectPenaltyDotsRemaining;
            outputStalled = true;
        } else if (scanlineObjectPenaltyAtX[scanlineOutputX] > 0) {
            objectPenaltyDotsRemaining =
                scanlineObjectPenaltyAtX[scanlineOutputX] - 1;
            scanlineObjectPenaltyAtX[scanlineOutputX] = 0;
            outputStalled = true;
        }

        if (!outputStalled)
            renderPixel(scanlineOutputX++);
    }

    if (ly < VISIBLE_SCANLINES &&
        dot == MODE2_DOTS + mode3Duration &&
        scanlineDrewWindow)
        ++windowLineCounter;

    if (dot == DOTS_PER_SCANLINE) {
        dot = 0;
        ++ly;
        if (ly == VISIBLE_SCANLINES)
            interrupts.requestInterrupt(INTERRUPT_VBLANK);
        else if (ly == SCANLINES_PER_FRAME) {
            ly = 0;
            windowLineCounter = 0;
        }
    }
    updateStatInterruptLine();
}

void DmgPpu::prepareScanline() {
    scanlineFineScroll = scrollX & 0x07;
    mode3Duration = MODE3_DOTS + scanlineFineScroll;
    scanlineOutputX = 0;
    scanlineWindowPenaltyPending = false;
    windowPenaltyDotsRemaining = 0;
    objectPenaltyDotsRemaining = 0;
    scanlineObjectPenaltyAtX.fill(0);
    scanlineBgColorIndices.fill(0);
    scanlineObjectColorIndices.fill(0);
    scanlineObjectUsesPalette1.fill(false);

    const bool bgWindowEnabled = (lcdControl & BG_WINDOW_ENABLE_MASK) != 0;
    const uint16_t bgTileMapOffset = (lcdControl & BG_TILE_MAP_MASK) ? 0x1C00 : 0x1800;
    const uint16_t windowTileMapOffset =
        (lcdControl & WINDOW_TILE_MAP_MASK) ? 0x1C00 : 0x1800;
    const bool windowEnabled = bgWindowEnabled && (lcdControl & WINDOW_ENABLE_MASK) &&
                               ly >= windowY && windowX <= 166;
    const int windowLeft = static_cast<int>(windowX) - 7;
    scanlineDrewWindow = false;
    if (windowEnabled && windowLeft < static_cast<int>(FRAME_WIDTH)) {
        scanlineWindowStartX = static_cast<uint8_t>(std::max(0, windowLeft));
        scanlineWindowPenaltyPending = true;
        mode3Duration += WINDOW_PENALTY_DOTS;
    }

    for (size_t x = 0; x < FRAME_WIDTH; ++x) {
        uint8_t bgColorIndex = 0;
        if (bgWindowEnabled) {
            const bool useWindow = windowEnabled && static_cast<int>(x) >= windowLeft;
            if (useWindow) {
                const uint8_t windowPixelX = static_cast<uint8_t>(
                    static_cast<int>(x) - windowLeft);
                bgColorIndex = getTileMapPixel(windowTileMapOffset, windowPixelX,
                                               windowLineCounter);
                scanlineDrewWindow = true;
            } else {
                bgColorIndex = getTileMapPixel(
                    bgTileMapOffset,
                    static_cast<uint8_t>(scrollX + static_cast<uint8_t>(x)),
                    static_cast<uint8_t>(scrollY + ly));
            }
        }
        scanlineBgColorIndices[x] = bgColorIndex;
    }

    if ((lcdControl & OBJECT_ENABLE_MASK) != 0)
        prepareObjects();
}

void DmgPpu::renderPixel(size_t screenX) {
    const size_t frameOffset = static_cast<size_t>(ly) * FRAME_WIDTH + screenX;
    frameBuffer[frameOffset] =
        applyPalette(bgPalette, scanlineBgColorIndices[screenX]);

    const uint8_t objectColorIndex = scanlineObjectColorIndices[screenX];
    if (objectColorIndex != 0) {
        const uint8_t palette = scanlineObjectUsesPalette1[screenX]
                                    ? objectPalette1
                                    : objectPalette0;
        frameBuffer[frameOffset] = applyPalette(palette, objectColorIndex);
    }
}

uint8_t DmgPpu::getTilePixel(uint8_t tileIndex, uint8_t x, uint8_t y,
                             bool unsignedTileData) const {
    int tileDataOffset;
    if (unsignedTileData) {
        tileDataOffset = static_cast<int>(tileIndex) * 16;
    } else {
        const int signedTileIndex = tileIndex < 0x80 ? tileIndex : tileIndex - 0x100;
        tileDataOffset = 0x1000 + signedTileIndex * 16;
    }

    const size_t rowOffset = static_cast<size_t>(tileDataOffset) + y * 2;
    const uint8_t bit = static_cast<uint8_t>(7 - x);
    const uint8_t lowBit = (videoRam[rowOffset] >> bit) & 0x01;
    const uint8_t highBit = (videoRam[rowOffset + 1] >> bit) & 0x01;
    return static_cast<uint8_t>(lowBit | (highBit << 1));
}

uint8_t DmgPpu::getTileMapPixel(uint16_t tileMapOffset, uint8_t x, uint8_t y) const {
    const size_t mapIndex = static_cast<size_t>(y / 8) * 32 + x / 8;
    const uint8_t tileIndex = videoRam[tileMapOffset + mapIndex];
    return getTilePixel(tileIndex, x % 8, y % 8,
                        (lcdControl & TILE_DATA_SELECT_MASK) != 0);
}

void DmgPpu::prepareObjects() {
    struct Object {
        size_t oamIndex;
        uint8_t x;
    };

    const int objectHeight = (lcdControl & OBJECT_SIZE_MASK) ? 16 : 8;
    std::array<Object, MAX_OBJECTS_PER_SCANLINE> selected{};
    size_t selectedCount = 0;

    // The ten-object limit is selected in OAM order, before X-priority is applied.
    for (size_t index = 0; index < OBJECT_COUNT && selectedCount < selected.size(); ++index) {
        const size_t base = index * OBJECT_ENTRY_SIZE;
        const int objectTop = static_cast<int>(objectRam[base]) - 16;
        if (static_cast<int>(ly) < objectTop || static_cast<int>(ly) >= objectTop + objectHeight)
            continue;
        selected[selectedCount++] = {index, objectRam[base + 1]};
    }

    std::sort(selected.begin(), selected.begin() + selectedCount,
              [](const Object &left, const Object &right) {
                  return left.x == right.x ? left.oamIndex < right.oamIndex
                                           : left.x < right.x;
              });

    const bool bgWindowEnabled = (lcdControl & BG_WINDOW_ENABLE_MASK) != 0;
    const bool windowEnabled = bgWindowEnabled &&
                               (lcdControl & WINDOW_ENABLE_MASK) &&
                               ly >= windowY && windowX <= 166;
    const int windowLeft = static_cast<int>(windowX) - 7;
    // Track BG and window tile columns separately. The first object that
    // interrupts a tile fetch pays the alignment wait; later objects in that
    // already-fetched tile pay only the fixed object-fetch cost.
    std::array<bool, 64> fetchedBackgroundTiles{};

    for (size_t selectedIndex = 0; selectedIndex < selectedCount;
         ++selectedIndex) {
        const Object &object = selected[selectedIndex];
        const int objectLeft = static_cast<int>(object.x) - 8;
        if (object.x >= FRAME_WIDTH + 8 && object.x != 0)
            continue;

        uint16_t penalty = 0;
        if (object.x == 0) {
            // An OBJ at X=0 is fully off-screen, but the DMG still incurs the
            // fixed penalty while attempting to fetch it.
            penalty = 11;
            scanlineObjectPenaltyAtX[0] += penalty;
        } else {
            const int firstVisibleX = std::max(0, objectLeft);
            const bool firstPixelUsesWindow =
                windowEnabled && objectLeft >= windowLeft;

            uint8_t tilePixelX;
            size_t fetchedTileIndex;
            if (firstPixelUsesWindow) {
                const uint8_t windowPixelX = static_cast<uint8_t>(
                    objectLeft - windowLeft);
                tilePixelX = windowPixelX & 0x07;
                fetchedTileIndex = 32 + (windowPixelX >> 3);
            } else {
                const uint8_t backgroundPixelX = static_cast<uint8_t>(
                    scrollX + static_cast<uint8_t>(objectLeft));
                tilePixelX = backgroundPixelX & 0x07;
                fetchedTileIndex = backgroundPixelX >> 3;
            }

            penalty = 6;
            if (!fetchedBackgroundTiles[fetchedTileIndex]) {
                fetchedBackgroundTiles[fetchedTileIndex] = true;
                const int backgroundPixelsToRight = 7 - tilePixelX;
                penalty = static_cast<uint16_t>(
                    penalty + std::max(0, backgroundPixelsToRight - 2));
            }

            const size_t stallX = static_cast<size_t>(firstVisibleX);
            // Multiple objects can begin at one output pixel; their stalls
            // occur consecutively before that pixel is emitted.
            scanlineObjectPenaltyAtX[stallX] += penalty;
        }

        mode3Duration = static_cast<uint16_t>(mode3Duration + penalty);
    }

    for (size_t screenX = 0; screenX < FRAME_WIDTH; ++screenX) {
        for (size_t selectedIndex = 0; selectedIndex < selectedCount; ++selectedIndex) {
            const Object &object = selected[selectedIndex];
            const size_t base = object.oamIndex * OBJECT_ENTRY_SIZE;
            const int objectLeft = static_cast<int>(object.x) - 8;
            int pixelX = static_cast<int>(screenX) - objectLeft;
            if (pixelX < 0 || pixelX >= 8)
                continue;

            const uint8_t attributes = objectRam[base + 3];
            int pixelY = static_cast<int>(ly) -
                         (static_cast<int>(objectRam[base]) - 16);
            if (attributes & 0x40)
                pixelY = objectHeight - 1 - pixelY;
            if (attributes & 0x20)
                pixelX = 7 - pixelX;

            uint8_t tileIndex = objectRam[base + 2];
            if (objectHeight == 16) {
                tileIndex &= 0xFE;
                tileIndex = static_cast<uint8_t>(tileIndex + pixelY / 8);
                pixelY %= 8;
            }

            const uint8_t colorIndex = getTilePixel(tileIndex,
                                                    static_cast<uint8_t>(pixelX),
                                                    static_cast<uint8_t>(pixelY), true);
            if (colorIndex == 0)
                continue;

            const bool behindBackground = (attributes & 0x80) != 0;
            if (!(behindBackground && (lcdControl & BG_WINDOW_ENABLE_MASK) &&
                  scanlineBgColorIndices[screenX] != 0)) {
                scanlineObjectColorIndices[screenX] = colorIndex;
                scanlineObjectUsesPalette1[screenX] =
                    (attributes & 0x10) != 0;
            }
            // A higher-priority opaque object still masks lower-priority objects
            // when its behind-BG flag leaves the background visible.
            break;
        }
    }
}

uint8_t DmgPpu::applyPalette(uint8_t palette, uint8_t colorIndex) {
    return static_cast<uint8_t>((palette >> (colorIndex * 2)) & 0x03);
}

}
