#include <cstdlib>
#include <iostream>

#include "dmg_ppu.hpp"
#include "interrupt_registers.hpp"

namespace {

using namespace LOTUSGB;

constexpr uint16_t LCDC = 0xFF40;
constexpr uint16_t STAT = 0xFF41;
constexpr uint16_t LY = 0xFF44;
constexpr uint16_t LYC = 0xFF45;
constexpr uint16_t VRAM = 0x8000;
constexpr uint16_t OAM = 0xFE00;
constexpr uint16_t IF = 0xFF0F;
constexpr uint16_t BGP = 0xFF47;
constexpr uint16_t OBP0 = 0xFF48;
constexpr uint16_t OBP1 = 0xFF49;
constexpr uint16_t SCX = 0xFF43;
constexpr uint16_t WX = 0xFF4B;
constexpr uint16_t WY = 0xFF4A;

constexpr uint8_t STAT_MODE0_INTERRUPT = 0x08;
constexpr uint8_t STAT_MODE2_INTERRUPT = 0x20;
constexpr uint8_t STAT_LYC_INTERRUPT = 0x40;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void writeTileRow(DmgPpu &ppu, uint8_t tile, uint8_t row, uint8_t low, uint8_t high) {
    const uint16_t address = static_cast<uint16_t>(VRAM + tile * 16 + row * 2);
    ppu.write(address, low);
    ppu.write(static_cast<uint16_t>(address + 1), high);
}

void renderFirstScanline(DmgPpu &ppu, uint8_t lcdc) {
    ppu.write(LCDC, lcdc);
    ppu.advanceTCycles(456);
}

void testDisabledPpuRegistersAndVideoMemory() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    expect(ppu.read(LCDC) == 0, "LCD starts disabled in the synthetic DMG reset state");
    expect(ppu.read(STAT) == 0x84,
           "STAT reports LCD disabled mode 0 and LY=LYC at reset");
    ppu.write(VRAM, 0xA5);
    ppu.write(OAM, 0x5A);
    expect(ppu.read(VRAM) == 0xA5, "VRAM is accessible while LCD is disabled");
    expect(ppu.read(OAM) == 0x5A, "OAM is accessible while LCD is disabled");

    ppu.write(LY, 0x42);
    expect(ppu.read(LY) == 0, "LY ignores writes");
}

void testVisibleLineModesAndVideoMemoryAccess() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    ppu.write(VRAM, 0x12);
    ppu.write(OAM, 0x34);
    ppu.write(LCDC, 0x80);
    ppu.advanceTCycles(454); // Check normal scanlines after the LCD startup line.
    expect((ppu.read(STAT) & 0x03) == 2, "normal visible line begins in mode 2");
    expect(ppu.read(VRAM) == 0x12, "VRAM is accessible during mode 2");
    expect(ppu.read(OAM) == 0xFF, "OAM reads are blocked during mode 2");
    ppu.write(OAM, 0x56);

    ppu.advanceTCycles(79);
    expect((ppu.read(STAT) & 0x03) == 2, "mode 2 lasts 80 dots");
    ppu.advanceTCycles(1);
    expect((ppu.read(STAT) & 0x03) == 3, "mode 3 begins after 80 dots");
    expect(ppu.read(VRAM) == 0xFF, "VRAM reads are blocked during mode 3");
    expect(ppu.read(OAM) == 0xFF, "OAM reads are blocked during mode 3");
    ppu.write(VRAM, 0x99);
    ppu.write(OAM, 0x88);

    ppu.advanceTCycles(171);
    expect((ppu.read(STAT) & 0x03) == 3, "mode 3 lasts at least 172 dots");
    ppu.advanceTCycles(1);
    expect((ppu.read(STAT) & 0x03) == 0, "mode 0 follows the initial 172-dot mode 3");
    expect(ppu.read(VRAM) == 0x12, "blocked mode 3 write does not alter VRAM");
    expect(ppu.read(OAM) == 0x34, "blocked mode 2 or 3 writes do not alter OAM");

    ppu.advanceTCycles(201);
    expect(ppu.read(LY) == 1, "LY stays on the current line before dot 454");
    ppu.advanceTCycles(1);
    expect(ppu.read(LY) == 2, "readable LY increments two dots before mode 2");
    expect((ppu.read(STAT) & 3) == 0, "mode 0 remains active at the early LY edge");
    ppu.advanceTCycles(2);
    expect((ppu.read(STAT) & 0x03) == 2, "the next visible line begins in mode 2");
}

void testLcdStartupLineBeginsInModeZero() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    ppu.write(OAM, 0x34);
    ppu.write(VRAM, 0x12);
    ppu.write(STAT, STAT_MODE2_INTERRUPT);
    ppu.write(LCDC, 0x80);
    expect((ppu.read(STAT) & 3) == 0, "LCD startup line begins in mode 0");
    expect(!(interrupts.read(IF) & INTERRUPT_LCD_STAT),
           "LCD startup does not generate a mode 2 interrupt");
    expect(ppu.read(OAM) == 0x34 && ppu.read(VRAM) == 0x12,
           "OAM and VRAM are accessible in startup mode 0");
    ppu.write(OAM, 0x56);
    ppu.advanceTCycles(77);
    expect((ppu.read(STAT) & 3) == 0, "startup mode 0 lasts until dot 78");
    ppu.advanceTCycles(1);
    expect((ppu.read(STAT) & 3) == 3, "startup line proceeds directly to mode 3");
    expect(ppu.read(OAM) == 0xff && ppu.read(VRAM) == 0xff,
           "startup mode 3 restricts OAM and VRAM access");
    ppu.advanceTCycles(171);
    expect((ppu.read(STAT) & 3) == 3, "startup mode 3 lasts 172 dots");
    ppu.advanceTCycles(1);
    expect((ppu.read(STAT) & 3) == 0, "startup HBlank begins at elapsed dot 250");
    ppu.advanceTCycles(201);
    expect(ppu.read(LY) == 0, "startup LY remains zero through elapsed dot 451");
    ppu.advanceTCycles(1);
    expect(ppu.read(LY) == 1, "startup LY advances at elapsed dot 452");
    expect((ppu.read(STAT) & 3) == 0, "startup LY edge precedes the mode 2 edge");
    ppu.advanceTCycles(2);
    expect((ppu.read(STAT) & 3) == 2, "following line uses normal mode 2");
    expect(interrupts.read(IF) & INTERRUPT_LCD_STAT,
           "following line requests the first mode 2 interrupt");
    ppu.write(LCDC, 0);
    expect(ppu.read(OAM) == 0x56, "startup mode 0 OAM writes take effect");
    ppu.write(LCDC, 0x80);
    expect((ppu.read(STAT) & 3) == 0, "each LCD restart uses the startup mode");
}

void testLyTransitionSeparatesComparisonAndOamReadAccess() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    ppu.write(OAM, 0x34);
    ppu.write(LYC, 1);
    ppu.write(STAT, STAT_LYC_INTERRUPT);
    ppu.write(LCDC, 0x80);
    ppu.advanceTCycles(451);
    expect(ppu.read(LY) == 0 && ppu.read(OAM) == 0x34,
           "startup LY and OAM access remain unchanged before the transition");
    ppu.advanceTCycles(1);
    expect(ppu.read(LY) == 1 && (ppu.read(STAT) & 7) == 0,
           "readable LY advances before mode 2 and LYC comparison");
    expect(ppu.read(OAM) == 0xff,
           "OAM reads are blocked during the two-dot line transition");
    ppu.write(OAM, 0x56);
    expect(!(interrupts.read(IF) & INTERRUPT_LCD_STAT),
           "LYC interrupt waits for the comparison clock after the transition");
    ppu.advanceTCycles(2);
    expect((ppu.read(STAT) & 7) == 6,
           "mode 2 and LYC match become active after the transition");
    expect(interrupts.read(IF) & INTERRUPT_LCD_STAT,
           "LYC match requests its interrupt after the transition");
    ppu.write(LCDC, 0);
    expect(ppu.read(OAM) == 0x56,
           "OAM writes still pass at the early read-blocking edge");

    // The old line's comparison must also clear during the transition.
    ppu.write(LYC, 0);
    ppu.write(LCDC, 0x80);
    ppu.advanceTCycles(452);
    expect(!(ppu.read(STAT) & 4),
           "neither old nor new LY matches during the transition window");
}

void testVramReadBlockPrecedesModeThree() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    ppu.write(VRAM, 0x12);
    ppu.write(OAM, 0x34);
    ppu.write(LCDC, 0x80);
    ppu.advanceTCycles(454 + 77);
    expect(ppu.read(VRAM) == 0x12, "VRAM is readable before the early mode 3 edge");
    ppu.advanceTCycles(1);
    expect((ppu.read(STAT) & 3) == 2 && ppu.read(VRAM) == 0xff,
           "VRAM reads block two dots before STAT reports mode 3");
    expect(ppu.read(OAM) == 0xff, "OAM reads remain blocked at the Mode 2 tail");
    ppu.write(OAM, 0x56);
    ppu.write(VRAM, 0x56);
    ppu.advanceTCycles(2);
    ppu.write(OAM, 0x88);
    ppu.write(VRAM, 0x88);
    ppu.write(LCDC, 0);
    expect(ppu.read(VRAM) == 0x56,
           "VRAM writes pass at the early read edge but block in mode 3");
    expect(ppu.read(OAM) == 0x56,
           "OAM writes pass in the final two Mode 2 dots but block in Mode 3");
}

void testFineScrollAddsStartupDotsToMode3() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    writeTileRow(ppu, 1, 0, 0xFF, 0x00);
    for (uint16_t column = 0; column < 32; ++column)
        ppu.write(static_cast<uint16_t>(0x9800 + column), 1);
    ppu.write(SCX, 3);
    ppu.write(BGP, 0xE4);
    ppu.write(LCDC, 0x91);

    ppu.advanceTCycles(78);
    expect((ppu.read(STAT) & 0x03) == 3,
           "startup transfer begins before the fine-scroll discard");
    expect(ppu.getFrameBuffer()[0] == 0,
           "SCX fine scroll delays the first visible pixel by three dots");

    ppu.advanceTCycles(15); // Reach elapsed dot 93; the first pixel is due at dot 94.
    expect(ppu.getFrameBuffer()[0] == 0,
           "the first pixel remains pending before the fine-scroll delay ends");
    ppu.advanceTCycles(1);
    expect(ppu.getFrameBuffer()[0] == 1,
           "the first pixel is emitted after the SCX fine-scroll delay");

    ppu.advanceTCycles(158);
    expect((ppu.read(STAT) & 0x03) == 3,
           "SCX fine scroll keeps mode 3 active through the last output pixel");
    ppu.advanceTCycles(1);
    expect((ppu.read(STAT) & 0x03) == 0,
           "three fine-scroll dots extend mode 3 by three dots");
    expect(ppu.getFrameBuffer()[DmgPpu::FRAME_WIDTH - 1] == 1,
           "the final visible pixel is emitted as mode 3 ends");
}

void testVisibleWindowAddsSixDotsToMode3() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    writeTileRow(ppu, 1, 0, 0xFF, 0x00); // Background color index 1.
    writeTileRow(ppu, 2, 0, 0x00, 0xFF); // Window color index 2.
    for (uint16_t column = 0; column < 32; ++column) {
        ppu.write(static_cast<uint16_t>(0x9800 + column), 1);
        ppu.write(static_cast<uint16_t>(0x9C00 + column), 2);
    }
    ppu.write(BGP, 0xE4);
    ppu.write(WY, 0);
    ppu.write(WX, 9); // Window begins at screen x=2.
    ppu.write(LCDC, 0xF1); // Enable LCD, window, unsigned tiles, and BG.

    ppu.advanceTCycles(98);
    const auto &frame = ppu.getFrameBuffer();
    expect(frame[1] == 1,
           "background pixels are emitted before the window penalty");
    expect(frame[2] == 0,
           "the first window pixel waits for the six-dot fetcher penalty");

    ppu.advanceTCycles(1);
    expect(frame[2] == 2,
           "the first window pixel is emitted after six stalled dots");

    ppu.advanceTCycles(156);
    expect((ppu.read(STAT) & 0x03) == 3,
           "the window penalty keeps mode 3 active through dot 257");
    ppu.advanceTCycles(1);
    expect((ppu.read(STAT) & 0x03) == 0,
           "a visible window extends mode 3 by six dots");
    expect(frame[DmgPpu::FRAME_WIDTH - 1] == 2,
           "the final visible pixel uses the window tile map");
}

void testObjectFetchPenaltyStallsPixelsAndExtendsMode3() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    writeTileRow(ppu, 1, 0, 0xFF, 0x00); // Background color index 1.
    writeTileRow(ppu, 2, 0, 0x00, 0xFF); // Object color index 2.
    for (uint16_t column = 0; column < 32; ++column)
        ppu.write(static_cast<uint16_t>(0x9800 + column), 1);
    ppu.write(BGP, 0xE4);
    ppu.write(OBP0, 0xE4);
    ppu.write(OAM, 16);
    ppu.write(OAM + 1, 11); // Object begins at screen x=3.
    ppu.write(OAM + 2, 2);
    ppu.write(LCDC, 0x93);

    ppu.advanceTCycles(93);
    const auto &frame = ppu.getFrameBuffer();
    expect(frame[2] == 1,
           "background pixels before the object are emitted on schedule");
    expect(frame[3] == 0,
           "the object pixel waits for its fetch penalty");

    ppu.advanceTCycles(8);
    expect(frame[3] == 0,
           "the first object in a background tile incurs an eight-dot stall");
    ppu.advanceTCycles(1);
    expect(frame[3] == 2,
           "the object pixel is emitted after its fetch stall");

    ppu.advanceTCycles(155);
    expect((ppu.read(STAT) & 0x03) == 3,
           "the object penalty extends mode 3 through dot 259");
    ppu.advanceTCycles(1);
    expect((ppu.read(STAT) & 0x03) == 0,
           "mode 0 begins after the object penalty's final output dot");
}

void testObjectsInAnAlreadyFetchedTilePayOnlyTheFlatPenalty() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    writeTileRow(ppu, 1, 0, 0xFF, 0x00);
    writeTileRow(ppu, 2, 0, 0x00, 0xFF);
    for (uint16_t column = 0; column < 32; ++column)
        ppu.write(static_cast<uint16_t>(0x9800 + column), 1);
    ppu.write(BGP, 0xE4);
    ppu.write(OBP0, 0xE4);
    ppu.write(OAM, 16);
    ppu.write(OAM + 1, 11); // Screen x=3, first object in this BG tile.
    ppu.write(OAM + 2, 2);
    ppu.write(OAM + 4, 16);
    ppu.write(OAM + 5, 15); // Screen x=7, same BG tile.
    ppu.write(OAM + 6, 2);
    ppu.write(LCDC, 0x93);

    ppu.advanceTCycles(111);
    const auto &frame = ppu.getFrameBuffer();
    expect(frame[7] == 0,
           "a later object in the same fetched tile stalls its first pixel");
    ppu.advanceTCycles(1);
    expect(frame[7] == 2,
           "the later object pays the flat six-dot fetch penalty");

    ppu.advanceTCycles(151);
    expect((ppu.read(STAT) & 0x03) == 3,
           "both object penalties extend mode 3 through dot 265");
    ppu.advanceTCycles(1);
    expect((ppu.read(STAT) & 0x03) == 0,
           "mode 0 begins after both object stalls have elapsed");
}

void testObjectAtXZeroUsesTheFixedFetchPenalty() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    ppu.write(OAM, 16);
    ppu.write(OAM + 1, 0); // Fully off-screen, with the DMG's fixed penalty.
    ppu.write(LCDC, 0x93);

    ppu.advanceTCycles(260);
    expect((ppu.read(STAT) & 0x03) == 3,
           "an X=0 object keeps mode 3 active through dot 262");
    ppu.advanceTCycles(1);
    expect((ppu.read(STAT) & 0x03) == 0,
           "an X=0 object adds its fixed eleven-dot penalty");
}

void testVblankInterruptAndFrameRollover() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    ppu.write(LCDC, 0x80);

    ppu.advanceTCycles(456U * 144U);
    expect(ppu.read(LY) == 144, "VBlank begins at scanline 144");
    expect((ppu.read(STAT) & 0x03) == 1, "VBlank scanlines use mode 1");
    expect((interrupts.read(IF) & INTERRUPT_VBLANK) != 0,
           "entering VBlank requests the VBlank interrupt");

    ppu.advanceTCycles(456U * 10U);
    expect(ppu.read(LY) == 0, "LY wraps after the ten VBlank scanlines");
    expect((ppu.read(STAT) & 0x03) == 2, "the next frame begins in mode 2");
}

void testStatInterruptUsesRisingEdgeOfCombinedSources() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    ppu.write(STAT, STAT_MODE2_INTERRUPT | STAT_MODE0_INTERRUPT);
    ppu.write(LCDC, 0x80);
    expect((interrupts.read(IF) & INTERRUPT_LCD_STAT) != 0,
           "startup mode 0 requests an enabled STAT interrupt");

    interrupts.acknowledgeInterrupt(INTERRUPT_LCD_STAT);
    ppu.advanceTCycles(80);
    expect((interrupts.read(IF) & INTERRUPT_LCD_STAT) == 0,
           "the STAT line falls while mode 3 has no enabled source");
    ppu.advanceTCycles(172);
    expect((interrupts.read(IF) & INTERRUPT_LCD_STAT) != 0,
           "entering enabled mode 0 raises the combined STAT interrupt line");

    interrupts.acknowledgeInterrupt(INTERRUPT_LCD_STAT);
    ppu.advanceTCycles(204);
    expect((interrupts.read(IF) & INTERRUPT_LCD_STAT) == 0,
           "switching between enabled mode 0 and mode 2 keeps the combined line high");
}

void testLycCoincidenceAndStatInterrupt() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    ppu.write(LYC, 1);
    ppu.write(STAT, STAT_LYC_INTERRUPT);
    ppu.write(LCDC, 0x80);
    expect((ppu.read(STAT) & 0x04) == 0, "LYC flag is clear while LY differs");
    ppu.advanceTCycles(456);
    expect((ppu.read(STAT) & 0x04) != 0, "LYC flag is set when LY reaches LYC");
    expect((interrupts.read(IF) & INTERRUPT_LCD_STAT) != 0,
           "LY=LYC requests STAT interrupt when enabled");
}

void testLycComparisonStopsWhileLcdIsOff() {
    for (bool previousMatch : {false, true}) {
        for (bool nextMatch : {false, true}) {
            InterruptRegisters interrupts;
            DmgPpu ppu(interrupts);
            ppu.write(STAT, STAT_LYC_INTERRUPT);
            ppu.write(LCDC, 0x80);
            ppu.advanceTCycles(456U * 144U);
            ppu.write(LYC, previousMatch ? 144 : 1);
            ppu.write(LCDC, 0);
            interrupts.acknowledgeInterrupt(INTERRUPT_LCD_STAT);
            expect(ppu.read(LY) == 0, "disabling LCD resets LY");
            expect(bool(ppu.read(STAT) & 4) == previousMatch,
                   "LCD disable retains the previous LY comparison flag");
            ppu.write(LYC, nextMatch ? 0 : 1);
            ppu.advanceTCycles(456);
            expect(bool(ppu.read(STAT) & 4) == previousMatch,
                   "LYC writes and clock ticks do not compare while LCD is off");
            expect(!(interrupts.read(IF) & INTERRUPT_LCD_STAT),
                   "the stopped comparator does not request interrupts");
            ppu.write(LCDC, 0x80);
            expect(bool(ppu.read(STAT) & 4) == nextMatch,
                   "LCD enable resumes comparison against LY zero");
            expect(bool(interrupts.read(IF) & INTERRUPT_LCD_STAT) ==
                       (!previousMatch && nextMatch),
                   "LCD enable requests LYC interrupt only for a new match");
        }
    }
}

void testBackgroundTilesAndPaletteRenderToFrameBuffer() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    // Tile 1 starts with color indices 1, 2, then 0.
    writeTileRow(ppu, 1, 0, 0x80, 0x40);
    ppu.write(0x9800, 1);
    ppu.write(BGP, 0xE4); // Identity mapping from pixel color to DMG shade.
    renderFirstScanline(ppu, 0x91); // LCD on, unsigned tile data, BG enabled.

    const auto &frame = ppu.getFrameBuffer();
    expect(frame[0] == 1, "background bitplane produces color index 1");
    expect(frame[1] == 2, "background bitplanes combine into color index 2");
    expect(frame[2] == 0, "background zero bits produce color index 0");
}

void testBackgroundPaletteWriteAffectsOnlyFuturePixels() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    writeTileRow(ppu, 1, 0, 0xFF, 0x00); // Every pixel has color index 1.
    for (uint16_t column = 0; column < 32; ++column)
        ppu.write(static_cast<uint16_t>(0x9800 + column), 1);
    ppu.write(BGP, 0xE4); // Color index 1 maps to shade 1.
    ppu.write(LCDC, 0x91);

    // The first visible pixel is emitted at dot 93. Stop after eight pixels,
    // then change BGP before the next pixel is emitted.
    ppu.advanceTCycles(98);
    ppu.write(BGP, 0x0C); // Color index 1 now maps to shade 3.
    ppu.advanceTCycles(1);

    const auto &frame = ppu.getFrameBuffer();
    expect(frame[7] == 1,
           "a mid-scanline BGP write does not recolor pixels already emitted");
    expect(frame[8] == 3,
           "a mid-scanline BGP write affects the next emitted pixel");

    ppu.advanceTCycles(151);
    expect(frame[DmgPpu::FRAME_WIDTH - 1] == 3,
           "the updated BGP remains active for the rest of the scanline");
}

void testSignedBackgroundTileAddressing() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    // In signed tile mode, tile ID $FF addresses $8FF0 (tile -1 from $9000).
    ppu.write(0x8FF0, 0x80);
    ppu.write(0x8FF1, 0x00);
    ppu.write(0x9800, 0xFF);
    ppu.write(BGP, 0xE4);
    renderFirstScanline(ppu, 0x81); // LCD on, signed tile data, BG enabled.

    expect(ppu.getFrameBuffer()[0] == 1,
           "signed tile index addresses tile data relative to 0x9000");
}

void testWindowPositionAndWindowTileMap() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    // With WX=4 the window starts three pixels before the screen; pixel 0
    // therefore reads source pixel 3 from the window tile.
    writeTileRow(ppu, 0, 0, 0x10, 0x00);
    ppu.write(0x9C00, 0);
    ppu.write(BGP, 0xE4);
    ppu.write(WY, 0);
    ppu.write(WX, 4);
    renderFirstScanline(ppu, 0xF1); // Window map 9C00, window and BG enabled.

    const auto &frame = ppu.getFrameBuffer();
    expect(frame[0] == 1, "window uses WX minus 7 and clips at the left edge");
    expect(frame[3] == 0, "window pixels advance from the clipped source position");
}

void testObjectsUseTransparencyPaletteAndPriority() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    writeTileRow(ppu, 1, 0, 0xFF, 0x00); // BG color 1.
    writeTileRow(ppu, 2, 0, 0x00, 0xFF); // Object color 2.
    writeTileRow(ppu, 3, 0, 0xFF, 0xFF); // Object color 3.
    ppu.write(0x9800, 1);
    ppu.write(0x9801, 1);
    ppu.write(0x9804, 1);
    ppu.write(BGP, 0xE4);
    ppu.write(OBP0, 0xE4);
    ppu.write(OBP1, 0x00);

    // Object 0 is behind BG at x=32. At x=2, object 1 is transparent; at
    // x=4, object 2 (x=11) wins over object 3 (x=12) despite its later OAM index.
    ppu.write(OAM, 16);
    ppu.write(OAM + 1, 40);
    ppu.write(OAM + 2, 2);
    ppu.write(OAM + 3, 0x80);
    ppu.write(OAM + 4, 16);
    ppu.write(OAM + 5, 10);
    ppu.write(OAM + 6, 4); // Transparent tile.
    ppu.write(OAM + 7, 0);
    ppu.write(OAM + 8, 16);
    ppu.write(OAM + 9, 12);
    ppu.write(OAM + 10, 3);
    ppu.write(OAM + 11, 0x10); // Select OBP1.
    ppu.write(OAM + 12, 16);
    ppu.write(OAM + 13, 11);
    ppu.write(OAM + 14, 2);
    ppu.write(OAM + 15, 0);

    renderFirstScanline(ppu, 0x93); // LCD on, objects and BG enabled.

    const auto &frame = ppu.getFrameBuffer();
    expect(frame[2] == 1, "transparent object pixels reveal the BG");
    expect(frame[4] == 2, "leftmost object wins overlap on DMG");
    expect(frame[11] == 0, "object attribute selects OBP1 for that object's pixels");
    expect(frame[32] == 1, "object behind a nonzero BG leaves the BG visible");
}

void testSixteenPixelObjectsUsePairedTiles() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    // In 8x16 mode the low tile bit is ignored. On object row 8, tile 5
    // supplies the pixels for an object whose OAM Y is 25.
    writeTileRow(ppu, 5, 0, 0xFF, 0x00);
    ppu.write(BGP, 0xE4);
    ppu.write(OBP0, 0xE4);
    ppu.write(OAM, 25);
    ppu.write(OAM + 1, 8);
    ppu.write(OAM + 2, 5);
    ppu.write(OAM + 3, 0);
    ppu.write(LCDC, 0x87); // LCD, 8x16 objects, objects/BG on, signed BG tiles.
    ppu.advanceTCycles(456U * 17U + 252U);

    expect(ppu.getFrameBuffer()[17 * DmgPpu::FRAME_WIDTH] == 1,
           "8x16 object uses the paired tile selected by its vertical row");
}

}

int main() {
    testDisabledPpuRegistersAndVideoMemory();
    testVisibleLineModesAndVideoMemoryAccess();
    testLcdStartupLineBeginsInModeZero();
    testLyTransitionSeparatesComparisonAndOamReadAccess();
    testVramReadBlockPrecedesModeThree();
    testFineScrollAddsStartupDotsToMode3();
    testVisibleWindowAddsSixDotsToMode3();
    testObjectFetchPenaltyStallsPixelsAndExtendsMode3();
    testObjectsInAnAlreadyFetchedTilePayOnlyTheFlatPenalty();
    testObjectAtXZeroUsesTheFixedFetchPenalty();
    testVblankInterruptAndFrameRollover();
    testStatInterruptUsesRisingEdgeOfCombinedSources();
    testLycCoincidenceAndStatInterrupt();
    testLycComparisonStopsWhileLcdIsOff();
    testBackgroundTilesAndPaletteRenderToFrameBuffer();
    testBackgroundPaletteWriteAffectsOnlyFuturePixels();
    testSignedBackgroundTileAddressing();
    testWindowPositionAndWindowTileMap();
    testObjectsUseTransparencyPaletteAndPriority();
    testSixteenPixelObjectsUsePairedTiles();
    std::cout << "DMG PPU tests passed\n";
}
