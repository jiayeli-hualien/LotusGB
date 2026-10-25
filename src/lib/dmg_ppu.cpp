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
// After LY first reports 153, it compares as 153 four dots later, then as 0
// after a short blank interval. LY itself resets at that four-dot boundary.
constexpr uint16_t LAST_LINE_153_COMPARISON_END_DOT = 4;
constexpr uint16_t LAST_LINE_ZERO_COMPARISON_DOT = 8;
constexpr uint16_t MODE2_DOTS = 80;
constexpr uint8_t WINDOW_PENALTY_DOTS = 6;
constexpr uint8_t VISIBLE_SCANLINES = 144;
constexpr uint8_t SCANLINES_PER_FRAME = 154;
constexpr size_t OBJECT_ENTRY_SIZE = 4;
constexpr size_t OBJECT_COUNT = 40;
}

DmgPpu::DmgPpu(IInterruptController &interrupts) : interrupts(interrupts) {}

void DmgPpu::initializeDmgPostBootState() {
    startupScanline = false;
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
    windowYTriggered = windowY == ly;
    dot = 0;
    mode3.reset();
    frameBuffer.fill(0);
    lycCoincidence = true;
    statInterruptLine = false;
}

uint8_t DmgPpu::read(uint16_t address) {
    if (address >= VRAM_START && address <= VRAM_END) {
        if (lcdEnabled() && (getMode() == 3 ||
            (getMode() == 2 && dot >= MODE2_DOTS - 2)))
            return 0xFF;
        return videoRam[address - VRAM_START];
    }

    if (address >= OAM_START && address <= OAM_END) {
        const uint8_t mode = getMode();
        if (lcdEnabled() && (mode == 2 || mode == 3 ||
            (dot >= DOTS_PER_SCANLINE - 2 &&
             (ly + 1) % SCANLINES_PER_FRAME < VISIBLE_SCANLINES)))
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
            return reportedLy();
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
        // The final two Mode 2 dots permit OAM writes while reads remain
        // blocked; the LCD-on write test verifies this distinct bus window.
        if (!lcdEnabled() || (mode != 3 &&
            (mode != 2 || dot >= MODE2_DOTS - 2)))
            objectRam[address - OAM_START] = value;
        return;
    }

    switch (address) {
        case LCDC_ADDRESS: {
            const bool wasEnabled = lcdEnabled();
            lcdControl = value;
            if (wasEnabled != lcdEnabled()) {
                startupScanline = lcdEnabled();
                // LCD startup begins two dots into the normal scanline timeline.
                // Mooneye lcdon_timing-GS checks LY, mode and access windows.
                dot = startupScanline ? 2 : 0;
                ly = 0;
                windowLineCounter = 0;
                windowYTriggered = lcdEnabled() && windowY == 0;
                mode3.reset();
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
        return startupScanline ? 0 : 2;
    if (dot < MODE2_DOTS + mode3.duration)
        return 3;
    return 0;
}

uint8_t DmgPpu::reportedLy() const {
    if (ly == SCANLINES_PER_FRAME - 1)
        return 0;
    // LCD startup begins two dots late, so its first line keeps the dot-454
    // LY edge. Normal DMG lines expose the next LY at dot 452; the Mooneye
    // hblank_ly_scx_timing-GS ROM checks this against the Mode 0 STAT edge.
    const uint16_t readableLyTransitionDot = startupScanline
        ? DOTS_PER_SCANLINE - 2 : DOTS_PER_SCANLINE - 4;
    return dot >= readableLyTransitionDot
        ? static_cast<uint8_t>((ly + 1) % SCANLINES_PER_FRAME) : ly;
}

bool DmgPpu::lcdEnabled() const {
    return (lcdControl & LCD_ENABLE_MASK) != 0;
}

bool DmgPpu::lyEqualsLyc() const {
    return lycCoincidence;
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
    // The LY comparator clock stops with the LCD. Retain both its flag and
    // interrupt edge history until the LCD starts comparing again.
    // Mooneye acceptance/ppu/stat_lyc_onoff.s documents all four transitions.
    if (!lcdEnabled())
        return;
    // During the two-dot LY transition neither the old nor the next line
    // compares equal (Mooneye lcdon_timing-GS checks LYC=0 and LYC=1).
    int comparisonLine = dot < DOTS_PER_SCANLINE - 2 ? ly : -1;
    if (ly == SCANLINES_PER_FRAME - 1) {
        // Line 153 briefly compares as 153 after readable LY is already 0,
        // then blanks comparison before switching to 0. Keep that match
        // through frame rollover: neither LY nor its comparator changes then.
        comparisonLine = -1;
        if (dot >= LAST_LINE_ZERO_COMPARISON_DOT)
            comparisonLine = 0;
        else if (dot < LAST_LINE_153_COMPARISON_END_DOT)
            comparisonLine = SCANLINES_PER_FRAME - 1;
    }
    lycCoincidence = comparisonLine == lyCompare;
    const bool signal = statInterruptSignal();
    if (signal && !statInterruptLine)
        interrupts.requestInterrupt(INTERRUPT_LCD_STAT);
    statInterruptLine = signal;
}

void DmgPpu::advanceDot() {
    ++dot;
    if (ly < VISIBLE_SCANLINES && dot == MODE2_DOTS)
        prepareScanline();

    if (ly < VISIBLE_SCANLINES)
        advanceMode3Pipeline();

    if (ly < VISIBLE_SCANLINES &&
        dot == MODE2_DOTS + mode3.duration &&
        mode3.drewWindow)
        ++windowLineCounter;

    if (dot == DOTS_PER_SCANLINE) {
        ++ly;
        startupScanline = false;
        dot = 0;
        if (ly == VISIBLE_SCANLINES) {
            windowYTriggered = false;
            // DMG also clocks the Mode 2 STAT source at VBlank entry,
            // although STAT's mode field becomes 1. Respect the combined
            // line's existing level, just as for ordinary STAT edges.
            if ((statInterruptEnable & STAT_MODE2_INTERRUPT_MASK) &&
                !statInterruptLine)
                interrupts.requestInterrupt(INTERRUPT_LCD_STAT);
            interrupts.requestInterrupt(INTERRUPT_VBLANK);
        }
        else if (ly == SCANLINES_PER_FRAME) {
            ly = 0;
            windowLineCounter = 0;
        }
        // DMG retains this condition until VBlank, even if LCDC.5 is clear.
        if (ly < VISIBLE_SCANLINES && ly == windowY)
            windowYTriggered = true;
    }
    updateStatInterruptLine();
}

void DmgPpu::advanceMode3Pipeline() {
    // Mode 3 has a 12-dot startup, plus the initial SCX fine-scroll discard,
    // before its visible output. Keep pixels on the PPU clock so palette
    // writes affect only pixels not yet sent to the LCD.
    const uint16_t firstFetchedPixelDot = MODE2_DOTS + 13;
    const uint16_t firstPixelDot = firstFetchedPixelDot + mode3.fineScroll;
    bool outputStalled = false;
    const bool outputReady = ly < VISIBLE_SCANLINES && mode3.outputX < FRAME_WIDTH &&
        dot >= firstPixelDot && dot < MODE2_DOTS + mode3.duration + 1;
    if (outputReady) {
        if (mode3.windowPenaltyPending &&
            mode3.outputX == mode3.windowStartX) {
            mode3.windowPenaltyPending = false;
            mode3.windowStarted = true;
            mode3.backgroundFifo.clear();
            mode3.windowPenaltyDotsRemaining = WINDOW_PENALTY_DOTS - 1;
            outputStalled = true;
        } else if (mode3.windowPenaltyDotsRemaining > 0) {
            --mode3.windowPenaltyDotsRemaining;
            outputStalled = true;
        } else if (mode3.objectFetcher.active()) {
            advanceBackgroundFetcherForObjectFetch();
            advanceActiveObjectFetch();
            outputStalled = true;
        } else {
            while (mode3.nextObjectFetchEvent < mode3.objectFetchEventCount &&
                   mode3.objectFetchEvents[mode3.nextObjectFetchEvent].startX ==
                       mode3.outputX) {
                ObjectFetchEvent &event =
                    mode3.objectFetchEvents[mode3.nextObjectFetchEvent];
                if (event.transferDots == 0) {
                    readObjectTilePlane(event, false);
                    readObjectTilePlane(event, true);
                    decodeObjectTileRow(event);
                    mode3.objectFifo.overlay(event);
                    ++mode3.nextObjectFetchEvent;
                    continue;
                }

                mode3.activeObjectFetchEvent = mode3.nextObjectFetchEvent;
                mode3.objectFetcher.begin(event);
                ++mode3.nextObjectFetchEvent;
                advanceBackgroundFetcherForObjectFetch();
                advanceActiveObjectFetch();
                outputStalled = true;
                break;
            }
        }
    }
    if (ly < VISIBLE_SCANLINES && dot >= MODE2_DOTS &&
        mode3.outputX < FRAME_WIDTH && !outputStalled)
        advanceBackgroundFetch();

    // SCX's fine offset discards pixels from the first fetched BG tile before
    // visible output. A window beginning at X=0 replaces that source first.
    if (dot >= firstFetchedPixelDot && dot < firstPixelDot &&
        !(mode3.drewWindow && mode3.windowStartX == 0)) {
        uint8_t discardedPixel = 0;
        mode3.backgroundFifo.pop(discardedPixel);
    }

    if (outputReady && !outputStalled)
        emitMode3Pixel();
}

void DmgPpu::advanceBackgroundFetcherForObjectFetch() {
    // OBJ arbitration advances BG fetching until a row is ready to push and
    // the BG FIFO already contains pixels, then waits for the object fetch.
    if (mode3.fetcher.phase == BackgroundFetcherState::Phase::Push &&
        mode3.backgroundFifo.size > 0)
        return;

    advanceBackgroundFetch();
}

void DmgPpu::advanceActiveObjectFetch() {
    ObjectFetchEvent &event =
        mode3.objectFetchEvents[mode3.activeObjectFetchEvent];
    switch (mode3.objectFetcher.advanceOneDot()) {
    case ObjectFetcherState::FetchAction::None:
        break;
    case ObjectFetcherState::FetchAction::ReadLowPlane:
        readObjectTilePlane(event, false);
        break;
    case ObjectFetcherState::FetchAction::ReadHighPlane:
        readObjectTilePlane(event, true);
        decodeObjectTileRow(event);
        mode3.objectFifo.overlay(event);
        break;
    }
}

void DmgPpu::emitMode3Pixel() {
    uint8_t backgroundColorIndex = 0;
    ObjectPixel objectPixel;
    if (mode3.windowStarted) {
        // Window pixels retain the line-start sampling model while sharing
        // the same Mode 3 pixel-output and palette path as BG FIFO pixels.
        mode3.backgroundFifo.pop(backgroundColorIndex);
        backgroundColorIndex = mode3.windowColorIndices[mode3.outputX];
        mode3.objectFifo.popAndShift(objectPixel);
        renderPixel(mode3.outputX++, backgroundColorIndex, objectPixel);
    } else if (mode3.backgroundFifo.pop(backgroundColorIndex)) {
        mode3.objectFifo.popAndShift(objectPixel);
        renderPixel(mode3.outputX++, backgroundColorIndex, objectPixel);
    } else {
        // A visible pixel cannot leave the PPU until its FIFO entry exists.
        ++mode3.duration;
    }
}

void DmgPpu::prepareScanline() {
    mode3.reset();
    mode3.fineScroll = scrollX & 0x07;
    mode3.duration = MODE3_BASE_DOTS + mode3.fineScroll;

    const bool bgWindowEnabled = (lcdControl & BG_WINDOW_ENABLE_MASK) != 0;
    const uint16_t windowTileMapOffset =
        (lcdControl & WINDOW_TILE_MAP_MASK) ? 0x1C00 : 0x1800;
    const bool windowEnabled = bgWindowEnabled && (lcdControl & WINDOW_ENABLE_MASK) &&
                               windowYTriggered && windowX <= 166;
    const int windowLeft = static_cast<int>(windowX) - 7;
    if (windowEnabled && windowLeft < static_cast<int>(FRAME_WIDTH)) {
        mode3.windowStartX = static_cast<uint8_t>(std::max(0, windowLeft));
        mode3.windowPenaltyPending = true;
        mode3.duration += WINDOW_PENALTY_DOTS;
    }

    for (size_t x = 0; x < FRAME_WIDTH; ++x) {
        if (bgWindowEnabled && windowEnabled && static_cast<int>(x) >= windowLeft) {
            const uint8_t windowPixelX = static_cast<uint8_t>(
                static_cast<int>(x) - windowLeft);
            mode3.windowColorIndices[x] = getTileMapPixel(windowTileMapOffset,
                                                           windowPixelX,
                                                           windowLineCounter);
            mode3.drewWindow = true;
        }
    }

    // OAM has already been scanned for this line, so retain its object pixels
    // even when LCDC.1 currently hides them. A mid-Mode-3 write can enable the
    // object output again before its pixels are emitted.
    prepareObjects((lcdControl & OBJECT_ENABLE_MASK) != 0);
}

uint8_t DmgPpu::readBackgroundBitplane(bool high) const {
    int tileOffset;
    if (lcdControl & TILE_DATA_SELECT_MASK) {
        tileOffset = mode3.fetcher.tile * 16;
    } else {
        const int signedIndex = mode3.fetcher.tile < 0x80
                                    ? mode3.fetcher.tile : mode3.fetcher.tile - 0x100;
        tileOffset = 0x1000 + signedIndex * 16;
    }
    // On DMG, each bitplane samples SCY separately, while the tile number
    // remains latched from the preceding tile-map read.
    const uint8_t row = static_cast<uint8_t>(scrollY + ly) & 7;
    return videoRam[tileOffset + row * 2 + (high ? 1 : 0)];
}

void DmgPpu::advanceBackgroundFetch() {
    // Preserve the current six-dot startup and two-dot read/push stages while
    // making the fetcher phase explicit. OBJ stalls still pause this BG fetcher;
    // bus-driven BG/OBJ FIFO arbitration remains future work.
    auto &fetcher = mode3.fetcher;
    if (fetcher.startupDotsRemaining > 0) {
        --fetcher.startupDotsRemaining;
        return;
    }

    if (fetcher.phaseDots == 0) {
        switch (fetcher.phase) {
        case BackgroundFetcherState::Phase::TileMap: {
            const uint8_t sourceX = static_cast<uint8_t>(
                (scrollX & 0xF8) + fetcher.tileColumn * 8);
            const uint8_t sourceY = static_cast<uint8_t>(scrollY + ly);
            const uint16_t backgroundMap =
                (lcdControl & BG_TILE_MAP_MASK) ? 0x1C00 : 0x1800;
            fetcher.tile = videoRam[backgroundMap + (sourceY / 8) * 32 + sourceX / 8];
            break;
        }
        case BackgroundFetcherState::Phase::TileDataLow:
            fetcher.low = readBackgroundBitplane(false);
            break;
        case BackgroundFetcherState::Phase::TileDataHigh:
            fetcher.high = readBackgroundBitplane(true);
            break;
        case BackgroundFetcherState::Phase::Push:
            // The fetcher waits at Push until the FIFO can accept another row.
            // The FIFO stores raw color indices; palettes are read at output.
            if (!mode3.backgroundFifo.pushRow(fetcher.low, fetcher.high))
                return;
            break;
        }
    }

    ++fetcher.phaseDots;
    if (fetcher.phaseDots < 2)
        return;

    fetcher.phaseDots = 0;
    switch (fetcher.phase) {
    case BackgroundFetcherState::Phase::TileMap:
        fetcher.phase = BackgroundFetcherState::Phase::TileDataLow;
        break;
    case BackgroundFetcherState::Phase::TileDataLow:
        fetcher.phase = BackgroundFetcherState::Phase::TileDataHigh;
        break;
    case BackgroundFetcherState::Phase::TileDataHigh:
        fetcher.phase = BackgroundFetcherState::Phase::Push;
        break;
    case BackgroundFetcherState::Phase::Push:
        ++fetcher.tileColumn;
        fetcher.phase = BackgroundFetcherState::Phase::TileMap;
        break;
    }
}

void DmgPpu::renderPixel(size_t screenX, uint8_t backgroundColorIndex,
                         const ObjectPixel &objectPixel) {
    const size_t frameOffset = static_cast<size_t>(ly) * FRAME_WIDTH + screenX;
    const bool backgroundEnabled = (lcdControl & BG_WINDOW_ENABLE_MASK) != 0;
    if (!backgroundEnabled)
        backgroundColorIndex = 0;
    frameBuffer[frameOffset] =
        applyPalette(bgPalette, backgroundColorIndex);

    if ((lcdControl & OBJECT_ENABLE_MASK) != 0 && objectPixel.colorIndex != 0 &&
        !(objectPixel.behindBackground && backgroundEnabled &&
          backgroundColorIndex != 0)) {
        const uint8_t palette = objectPixel.usesPalette1
                                    ? objectPalette1
                                    : objectPalette0;
        frameBuffer[frameOffset] = applyPalette(palette, objectPixel.colorIndex);
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

void DmgPpu::prepareObjects(bool includeFetchPenalties) {
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
                               windowYTriggered && windowX <= 166;
    const int windowLeft = static_cast<int>(windowX) - 7;
    // Track BG and window tile columns separately. The first object that
    // interrupts a tile fetch pays the alignment wait; later objects in that
    // already-fetched tile pay only the fixed object-fetch cost.
    std::array<bool, 64> fetchedBackgroundTiles{};
    bool fetchedXZeroObject = false;
    bool initialObjectFetchOverlapUsed = false;

    for (size_t selectedIndex = 0; selectedIndex < selectedCount;
         ++selectedIndex) {
        const Object &object = selected[selectedIndex];
        const int objectLeft = static_cast<int>(object.x) - 8;
        if (object.x >= FRAME_WIDTH + 8 && object.x != 0)
            continue;

        ObjectFetchEvent event;
        event.startX = static_cast<uint8_t>(std::max(0, objectLeft));
        event.firstPixelIndex = static_cast<uint8_t>(std::max(0, -objectLeft));

        if (includeFetchPenalties) {
            uint16_t penalty = 0;
            event.transferDots = 6;
            if (object.x == 0) {
                // An OBJ at X=0 is fully off-screen, but the DMG still fetches it.
                // The initial fetch costs 11 dots; additional X=0 objects at the
                // same fetch position pay only the flat six-dot fetch cost.
                penalty = fetchedXZeroObject ? 6 : 11;
                event.alignmentDots = static_cast<uint8_t>(penalty - event.transferDots);
                fetchedXZeroObject = true;
            } else {
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

                penalty = event.transferDots;
                if (!fetchedBackgroundTiles[fetchedTileIndex]) {
                    fetchedBackgroundTiles[fetchedTileIndex] = true;
                    const int backgroundPixelsToRight = 7 - tilePixelX;
                    event.alignmentDots = static_cast<uint8_t>(
                        std::max(0, backgroundPixelsToRight - 2));
                    penalty = static_cast<uint16_t>(penalty + event.alignmentDots);
                }
            }

            // The first object fetch overlaps one dot of the initial background
            // fetch phase; later object fetches do not share this overlap.
            if (!initialObjectFetchOverlapUsed) {
                --penalty;
                --event.transferDots;
                initialObjectFetchOverlapUsed = true;
            }

            mode3.duration = static_cast<uint16_t>(mode3.duration + penalty);
        }

        const size_t base = object.oamIndex * OBJECT_ENTRY_SIZE;
        const uint8_t attributes = objectRam[base + 3];
        int objectRow = static_cast<int>(ly) -
                        (static_cast<int>(objectRam[base]) - 16);
        if (attributes & 0x40)
            objectRow = objectHeight - 1 - objectRow;

        uint8_t tileIndex = objectRam[base + 2];
        if (objectHeight == 16) {
            tileIndex &= 0xFE;
            tileIndex = static_cast<uint8_t>(tileIndex + objectRow / 8);
            objectRow %= 8;
        }
        event.tileIndex = tileIndex;
        event.tileRow = static_cast<uint8_t>(objectRow);
        event.attributes = attributes;

        mode3.objectFetchEvents[mode3.objectFetchEventCount++] = event;
    }
}

void DmgPpu::readObjectTilePlane(ObjectFetchEvent &event, bool high) const {
    const size_t rowOffset = static_cast<size_t>(event.tileIndex) * 16 +
                             event.tileRow * 2 + (high ? 1 : 0);
    if (high)
        event.high = videoRam[rowOffset];
    else
        event.low = videoRam[rowOffset];
}

void DmgPpu::decodeObjectTileRow(ObjectFetchEvent &event) const {
    for (size_t sourcePixel = 0; sourcePixel < event.pixels.size(); ++sourcePixel) {
        const uint8_t bit = static_cast<uint8_t>(
            (event.attributes & 0x20) ? sourcePixel : 7 - sourcePixel);
        const uint8_t colorIndex = static_cast<uint8_t>(
            ((event.low >> bit) & 0x01) | (((event.high >> bit) & 0x01) << 1));
        event.pixels[sourcePixel] = {
            colorIndex,
            (event.attributes & 0x10) != 0,
            (event.attributes & 0x80) != 0};
    }
}

uint8_t DmgPpu::applyPalette(uint8_t palette, uint8_t colorIndex) {
    return static_cast<uint8_t>((palette >> (colorIndex * 2)) & 0x03);
}

}
