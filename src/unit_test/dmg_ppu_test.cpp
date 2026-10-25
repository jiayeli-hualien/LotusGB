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
constexpr uint16_t SCY = 0xFF42;
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

    ppu.advanceTCycles(199);
    expect(ppu.read(LY) == 1, "normal-line LY stays unchanged through dot 451");
    ppu.advanceTCycles(1);
    expect(ppu.read(LY) == 2, "normal-line LY advances at dot 452");
    expect((ppu.read(STAT) & 3) == 0, "mode 0 remains active at the early LY edge");
    ppu.advanceTCycles(4);
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

    ppu.advanceTCycles(7);
    expect(frame[3] == 0,
           "the first object fetch overlaps one dot with initial background fetch");
    ppu.advanceTCycles(1);
    expect(frame[3] == 2,
           "the object pixel is emitted after the seven-dot visible stall");

    ppu.advanceTCycles(155);
    expect((ppu.read(STAT) & 0x03) == 3,
           "the object penalty keeps mode 3 active through dot 258");
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

    ppu.advanceTCycles(110);
    const auto &frame = ppu.getFrameBuffer();
    expect(frame[7] == 0,
           "a later object in the same fetched tile stalls its first pixel");
    ppu.advanceTCycles(1);
    expect(frame[7] == 2,
           "the later object pays the flat six-dot fetch penalty");

    ppu.advanceTCycles(151);
    expect((ppu.read(STAT) & 0x03) == 3,
           "both object penalties keep mode 3 active through dot 264");
    ppu.advanceTCycles(1);
    expect((ppu.read(STAT) & 0x03) == 0,
           "mode 0 begins after both object stalls have elapsed");
}

void testObjectAtXZeroMode0Boundary() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    ppu.write(OAM, 16);
    ppu.write(OAM + 1, 0); // Fully off-screen, with the DMG's fixed penalty.
    ppu.write(LCDC, 0x93);

    ppu.advanceTCycles(259);
    expect((ppu.read(STAT) & 0x03) == 3,
           "an X=0 object keeps mode 3 active through dot 261");
    ppu.advanceTCycles(1);
    expect((ppu.read(STAT) & 0x03) == 0,
           "mode 0 starts at dot 262 for an X=0 object");
}

void testObjectsAtSameXRunSeparateFetchEvents() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    ppu.write(OAM, 16);
    ppu.write(OAM + 1, 0);
    ppu.write(OAM + 4, 16);
    ppu.write(OAM + 5, 0);
    ppu.write(LCDC, 0x93);

    ppu.advanceTCycles(265);
    expect((ppu.read(STAT) & 0x03) == 3,
           "both X=0 object fetches stall output at the same pixel position");
    ppu.advanceTCycles(1);
    expect((ppu.read(STAT) & 0x03) == 0,
           "same-position object fetch events consume their full combined duration");
}

void testTenObjectsAtXTenMatchMode0TimingBoundary() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    for (uint16_t index = 0; index < 10; ++index) {
        const uint16_t base = static_cast<uint16_t>(OAM + index * 4);
        ppu.write(base, 16);
        ppu.write(base + 1, 10);
    }
    ppu.write(LCDC, 0x93);

    // Mooneye expects a 15-M-cycle Mode 2-to-Mode 0 interval for ten X=10 objects.
    ppu.advanceTCycles(311);
    expect((ppu.read(STAT) & 0x03) == 3,
           "ten X=10 objects keep mode 3 active through dot 313");
    ppu.advanceTCycles(1);
    expect((ppu.read(STAT) & 0x03) == 0,
           "ten X=10 objects enter mode 0 at dot 314");
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

void advanceToReadableLine153(DmgPpu &ppu) {
    // LY changes ahead of the mode timeline; anchor observations to the
    // publicly visible counter, not the PPU's private line/dot fields.
    for (unsigned dots = 0; dots < 456U * 154U; ++dots) {
        if (ppu.read(LY) == 153)
            return;
        ppu.advanceTCycles(1);
    }
    expect(false, "PPU reaches readable line 153 within one frame");
}

void testLastVblankLineResetsLyBeforeModeEnds() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    ppu.write(LCDC, 0x80);
    advanceToReadableLine153(ppu);
    ppu.advanceTCycles(3);
    expect(ppu.read(LY) == 153, "LY still reads 153 before its four-dot reset boundary");
    ppu.advanceTCycles(1);
    expect(ppu.read(LY) == 0, "LY resets four dots after first becoming readable as 153");
    expect((ppu.read(STAT) & 3) == 1, "early LY zero does not end VBlank mode");
    interrupts.write(IF, 0);
    ppu.advanceTCycles(455); // LY now resets at the start of internal line 153.
    expect(ppu.read(LY) == 0 && (ppu.read(STAT) & 3) == 1,
           "the remainder of the final scanline is still LY zero in mode 1");
    expect((interrupts.read(IF) & INTERRUPT_VBLANK) == 0,
           "early LY reset does not generate another VBlank request");
    ppu.advanceTCycles(1);
    expect(ppu.read(LY) == 0 && (ppu.read(STAT) & 3) == 2,
           "next frame enters mode 2 only at the full scanline boundary");
}

void testLastVblankLineLycComparisonAndInterruptEdges() {
    for (uint8_t lyc : {uint8_t(0), uint8_t(153)}) {
        InterruptRegisters interrupts;
        DmgPpu ppu(interrupts);
        ppu.write(LYC, lyc);
        ppu.write(STAT, STAT_LYC_INTERRUPT);
        ppu.write(LCDC, 0x80);
        advanceToReadableLine153(ppu);
        interrupts.write(IF, 0);
        expect((ppu.read(STAT) & 4) == 0, "LY transition starts with coincidence clear");
        ppu.advanceTCycles(4);
        expect(ppu.read(LY) == 0 && bool(ppu.read(STAT) & 4) == (lyc == 153),
               "line 153 comparison is delayed relative to the readable LY reset");
        expect(bool(interrupts.read(IF) & INTERRUPT_LCD_STAT) == (lyc == 153),
               "LYC=153 requests its edge during the delayed comparison");
        interrupts.write(IF, 0);
        ppu.advanceTCycles(4);
        expect((ppu.read(STAT) & 4) == 0,
               "comparison has a blank interval between line 153 and line zero");
        expect((interrupts.read(IF) & INTERRUPT_LCD_STAT) == 0,
               "blank comparison interval does not request STAT");
        ppu.advanceTCycles(4);
        expect(bool(ppu.read(STAT) & 4) == (lyc == 0),
               "LYC comparison switches to zero during the last VBlank scanline");
        expect(bool(interrupts.read(IF) & INTERRUPT_LCD_STAT) == (lyc == 0),
               "LYC=0 requests STAT before VBlank mode ends");
        interrupts.write(IF, 0);
        ppu.advanceTCycles(448); // Finish line 153 from its dot-8 comparison edge.
        expect((ppu.read(STAT) & 3) == 2 && bool(ppu.read(STAT) & 4) == (lyc == 0),
               "line zero comparison survives the frame boundary without a false mismatch");
        expect((interrupts.read(IF) & INTERRUPT_LCD_STAT) == 0,
               "frame rollover does not repeat a continuous LYC=0 request");
    }
}

void testModeOneStatBlocksLastLineLycZeroEdge() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    ppu.write(LYC, 0);
    ppu.write(STAT, 0x10 | STAT_LYC_INTERRUPT);
    ppu.write(LCDC, 0x80);
    advanceToReadableLine153(ppu);
    interrupts.write(IF, 0);
    ppu.advanceTCycles(12);
    expect((ppu.read(STAT) & 4) != 0 && (ppu.read(STAT) & 3) == 1,
           "LYC=0 becomes true while the mode 1 source is active");
    expect((interrupts.read(IF) & INTERRUPT_LCD_STAT) == 0,
           "already-high mode 1 source blocks the LYC rising-edge request");
    ppu.advanceTCycles(446);
    expect((interrupts.read(IF) & INTERRUPT_LCD_STAT) == 0,
           "switching from mode 1 to a continuous LYC=0 source does not retrigger STAT");
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

void testModeTwoStatSourceAtVblankEntry() {
    for (uint8_t enabled : {uint8_t(0), STAT_MODE2_INTERRUPT,
                            uint8_t(STAT_MODE0_INTERRUPT | STAT_MODE2_INTERRUPT)}) {
        InterruptRegisters interrupts;
        DmgPpu ppu(interrupts);
        ppu.initializeDmgPostBootState();
        ppu.write(STAT, enabled);
        ppu.advanceTCycles(456U * 144U - 1);
        interrupts.acknowledgeInterrupt(INTERRUPT_LCD_STAT | INTERRUPT_VBLANK);
        expect((ppu.read(STAT) & 3) == 0,
               "the final visible scanline remains in HBlank before VBlank");
        ppu.advanceTCycles(1);
        expect((ppu.read(STAT) & 3) == 1,
               "VBlank entry reports mode 1 even for the Mode 2 STAT source");
        expect(interrupts.read(IF) & INTERRUPT_VBLANK,
               "VBlank interrupt is requested at the entry dot");
        expect(bool(interrupts.read(IF) & INTERRUPT_LCD_STAT) ==
                   (enabled == STAT_MODE2_INTERRUPT),
               "Mode 2 requests STAT at VBlank only if enabled and not blocked");
        interrupts.acknowledgeInterrupt(INTERRUPT_LCD_STAT | INTERRUPT_VBLANK);
        ppu.advanceTCycles(456);
        expect(!(interrupts.read(IF) & (INTERRUPT_LCD_STAT | INTERRUPT_VBLANK)),
               "later VBlank lines do not repeat the entry interrupt");
    }
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

void testBackgroundFifoPreservesPixelsAcrossTileMapWrap() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    writeTileRow(ppu, 1, 0, 0xFF, 0x00); // Color 1.
    writeTileRow(ppu, 2, 0, 0x00, 0xFF); // Color 2.
    ppu.write(0x981F, 1); // SCX=$F8 starts from the last tile in the map.
    ppu.write(0x9800, 2); // The next fetched tile wraps to map column zero.
    ppu.write(BGP, 0xE4);
    ppu.write(SCX, 0xF8);
    renderFirstScanline(ppu, 0x91);

    const auto &frame = ppu.getFrameBuffer();
    expect(frame[0] == 1 && frame[7] == 1,
           "the FIFO emits the final background-map tile in pixel order");
    expect(frame[8] == 2 && frame[15] == 2,
           "the FIFO continues with column zero after the tile map wraps");
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

void testCoarseScrollSamplesLaterTilesWithoutChangingFineDiscard() {
    for (uint8_t fine : {uint8_t{0}, uint8_t{3}}) {
        InterruptRegisters interrupts;
        DmgPpu ppu(interrupts);
        writeTileRow(ppu, 0, 0, 0xFF, 0x00); // Color 1.
        writeTileRow(ppu, 1, 0, 0x00, 0xFF); // Color 2.
        writeTileRow(ppu, 2, 0, 0xFF, 0xFF); // Color 3.
        writeTileRow(ppu, 3, 0, 0x00, 0x00); // Color 0.
        for (int x = 0; x < 32; ++x)
            ppu.write(0x9800 + x, x % 4);
        ppu.write(BGP, 0xE4);
        ppu.write(SCX, fine);
        ppu.write(LCDC, 0x91);
        ppu.advanceTCycles(94); // Dot 96; the second tile is already fetched.
        ppu.write(SCX, 0x0F); // Change coarse offset and low bits together.
        ppu.advanceTCycles(360);
        const auto &frame = ppu.getFrameBuffer();
        expect(frame[0] == 1, "already emitted pixels retain the old scroll");
        expect(frame[8 - fine] == 2,
               "already fetched tile retains its old coarse scroll");
        expect(frame[16 - fine] == 0,
               "later tile samples new SCX high bits and retains initial fine discard");
        expect(frame[24 - fine] == 1, "coarse scroll remains active on subsequent tiles");
    }
}

void testCoarseScrollWrittenDuringObjectFetchAffectsLaterTiles() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    writeTileRow(ppu, 1, 1, 0xFF, 0x00); // Tile 1 row 1: color 1.
    writeTileRow(ppu, 2, 1, 0x00, 0xFF); // Tile 2 row 1: color 2.
    writeTileRow(ppu, 3, 1, 0xFF, 0xFF); // Tile 3 row 1: color 3.
    ppu.write(0x9801, 1);
    ppu.write(0x9802, 2);
    ppu.write(0x9803, 3);
    ppu.write(BGP, 0xE4);
    ppu.write(OAM, 17);     // Select this object on visible line 1.
    ppu.write(OAM + 1, 1);  // Its fetch stalls the first visible pixel.
    ppu.write(LCDC, 0x93);

    ppu.advanceTCycles(454); // Finish LCD startup at line 1, dot 0.
    ppu.advanceTCycles(95);  // Dot 95 is inside the first object's Mode 3 stall.
    expect(ppu.read(LY) == 1 && (ppu.read(STAT) & 0x03) == 3,
           "the SCX write occurs during line 1 Mode 3 object fetch");
    ppu.write(SCX, 8);
    ppu.advanceTCycles(456 - 95);

    const auto &frame = ppu.getFrameBuffer();
    const size_t line = DmgPpu::FRAME_WIDTH;
    expect(frame[line + 8] == 2,
           "the first tile fetched after an object stall samples new SCX high bits");
    expect(frame[line + 16] == 3,
           "later tile fetches retain the updated coarse scroll after an object stall");
}

void testObjectFetchStopsBackgroundCatchUpWhenBgFifoHasPixels() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    writeTileRow(ppu, 1, 1, 0xFF, 0x00); // Row 1: color 1.
    writeTileRow(ppu, 1, 2, 0x00, 0xFF); // Row 2: color 2.
    ppu.write(0x9801, 1); // Tile 1 starts at screen x=8.
    ppu.write(BGP, 0xE4);
    ppu.write(OAM, 17); // Object starts on visible line 1.
    ppu.write(OAM + 1, 11); // Its transparent pixels start at screen x=3.
    ppu.write(LCDC, 0x93);

    ppu.advanceTCycles(454); // Finish LCD startup at line 1, dot 0.
    ppu.advanceTCycles(100); // Dot 100 is inside the object's fetch event.
    expect(ppu.read(LY) == 1 && (ppu.read(STAT) & 0x03) == 3,
           "the SCY write occurs during line 1 Mode 3 object fetch");
    ppu.write(SCY, 1);
    ppu.advanceTCycles(456 - 100);

    const auto &frame = ppu.getFrameBuffer();
    expect(frame[DmgPpu::FRAME_WIDTH + 8] == 2,
           "the BG fetcher waits for a non-empty FIFO during OBJ arbitration");
}

void testBackgroundBitplanesSampleScySeparately() {
    for (unsigned writeDot : {87U, 89U, 91U}) {
        InterruptRegisters interrupts;
        DmgPpu ppu(interrupts);
        writeTileRow(ppu, 0, 0, 0xFF, 0x00); // Row 0: color 1.
        writeTileRow(ppu, 0, 1, 0x00, 0xFF); // Row 1: color 2.
        ppu.write(BGP, 0xE4);
        ppu.write(LCDC, 0x91);
        ppu.advanceTCycles(writeDot - 2); // Startup begins at internal dot 2.
        ppu.write(SCY, 1);
        ppu.advanceTCycles(456 - writeDot);
        const uint8_t expected = writeDot == 87 ? 2 : writeDot == 89 ? 3 : 1;
        expect(ppu.getFrameBuffer()[0] == expected,
               "SCY changes before/between/after bitplane reads affect only unread data");
        expect(ppu.getFrameBuffer()[8] == 2,
               "subsequent tile bitplanes use the new SCY row");
    }
}

void testScyTileNumberRemainsLatchedBetweenFetchStages() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    writeTileRow(ppu, 0, 0, 0xFF, 0x00);
    writeTileRow(ppu, 1, 0, 0x00, 0xFF);
    for (int x = 0; x < 32; ++x)
        ppu.write(0x9820 + x, 1); // Tile-map row 1 selects a different tile.
    ppu.write(BGP, 0xE4);
    ppu.write(LCDC, 0x91);
    ppu.advanceTCycles(85); // Dot 87, after the first tile-map read.
    ppu.write(SCY, 8);
    ppu.advanceTCycles(369);
    expect(ppu.getFrameBuffer()[0] == 1,
           "SCY does not replace a tile number already fetched from the map");
    expect(ppu.getFrameBuffer()[8] == 2,
           "the next tile-map read samples the new SCY tile row");
}

void testBackgroundTileMapSelectIsSampledAtEachTileFetch() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    for (uint8_t row = 0; row < 8; ++row) {
        writeTileRow(ppu, 0, row, 0x00, 0x00); // Color 0.
        writeTileRow(ppu, 1, row, 0xFF, 0xFF); // Color 3.
    }
    for (int x = 0; x < 32; ++x) {
        ppu.write(0x9800 + x, 0); // Map 0 selects the white tile.
        ppu.write(0x9C00 + x, 1); // Map 1 selects the dark tile.
    }
    ppu.write(BGP, 0xE4);
    ppu.write(LCDC, 0x91); // LCD on, unsigned tile data, map 0, BG on.
    ppu.advanceTCycles(91); // Dot 93: tile 0 is fetched; tile 1 is not.
    ppu.write(LCDC, 0x99); // Select map 1 before tile 1's map fetch at dot 94.
    ppu.advanceTCycles(363);

    const auto &frame = ppu.getFrameBuffer();
    expect(frame[0] == 0, "already fetched tile retains its original map entry");
    expect(frame[8] == 3, "later tile fetch samples the updated LCDC map select");
}

void testObjectBehindBackgroundUsesFetchedScrollPixels() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    writeTileRow(ppu, 0, 0, 0xFF, 0xFF); // Opaque background.
    writeTileRow(ppu, 1, 0, 0x00, 0x00); // Transparent background color 0.
    writeTileRow(ppu, 2, 0, 0xFF, 0x00); // Object color 1.
    ppu.write(0x9803, 1);
    ppu.write(OAM, 16);
    ppu.write(OAM + 1, 24); // Object begins at screen x=16.
    ppu.write(OAM + 2, 2);
    ppu.write(OAM + 3, 0x80); // Behind nonzero BG pixels.
    ppu.write(BGP, 0xE4);
    ppu.write(OBP0, 0xE4);
    ppu.write(LCDC, 0x93);
    ppu.advanceTCycles(94);
    ppu.write(SCX, 8);
    ppu.advanceTCycles(360);
    expect(ppu.getFrameBuffer()[16] == 1,
           "object priority compares the fetched background, not a stale line snapshot");
}

void testBackgroundEnableIsSampledForEachOutputPixel() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    for (uint8_t row = 0; row < 8; ++row)
        writeTileRow(ppu, 0, row, 0x00, 0xFF); // Color 2.
    for (int x = 0; x < 32; ++x)
        ppu.write(0x9800 + x, 0);
    ppu.write(BGP, 0xE4);
    ppu.write(LCDC, 0x91); // LCD on, unsigned tile data, BG enabled.

    ppu.advanceTCycles(90); // Dot 92; visible output begins on the next dot.
    ppu.write(LCDC, 0x90); // Disable BG for the first three output pixels.
    ppu.advanceTCycles(3);
    ppu.write(LCDC, 0x91);
    ppu.advanceTCycles(361);

    const auto &frame = ppu.getFrameBuffer();
    expect(frame[0] == 0 && frame[1] == 0 && frame[2] == 0,
           "clearing LCDC.0 makes upcoming DMG background pixels color zero");
    expect(frame[3] == 2,
           "setting LCDC.0 restores background output on later pixels");
}

void testBackgroundCanBeEnabledMidScanlineAfterStartingDisabled() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    for (uint8_t row = 0; row < 8; ++row)
        writeTileRow(ppu, 0, row, 0x00, 0xFF); // Color 2.
    for (int x = 0; x < 32; ++x)
        ppu.write(0x9800 + x, 0);
    ppu.write(BGP, 0xE4);
    ppu.write(LCDC, 0x90); // LCD on, unsigned tile data, BG initially disabled.

    ppu.advanceTCycles(90); // Dot 92; tile data has been fetched before output.
    ppu.advanceTCycles(3); // Output the first three pixels while BG is hidden.
    ppu.write(LCDC, 0x91); // Re-enable BG at dot 95.
    ppu.advanceTCycles(361);

    const auto &frame = ppu.getFrameBuffer();
    expect(frame[0] == 0 && frame[1] == 0 && frame[2] == 0,
           "background pixels stay hidden while LCDC.0 is clear");
    expect(frame[3] == 2,
           "reenabling LCDC.0 reveals background fetched during the hidden interval");
}

void testObjectEnableIsSampledForEachOutputPixel() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    writeTileRow(ppu, 1, 0, 0xFF, 0x00); // Object color 1.
    ppu.write(OAM, 16);
    ppu.write(OAM + 1, 8); // Object begins at screen x=0.
    ppu.write(OAM + 2, 1);
    ppu.write(OAM + 3, 0);
    ppu.write(OBP0, 0xE4);
    ppu.write(LCDC, 0x93); // LCD on, objects and BG enabled.

    // The x=0 fetch has a nominal 11-dot cost, with one dot overlapping the
    // initial background fetch. Hide objects through the first two emitted
    // pixels, then re-enable them for later ones.
    ppu.advanceTCycles(90); // Dot 92, just before the first output dot.
    ppu.write(LCDC, 0x91); // Disable objects, keep LCD and BG on.
    ppu.advanceTCycles(12); // Dot 104, after output pixels 0 and 1.
    ppu.write(LCDC, 0x93);
    ppu.advanceTCycles(1);

    const auto &frame = ppu.getFrameBuffer();
    expect(frame[0] == 0 && frame[1] == 0,
           "clearing LCDC.1 hides object pixels not yet emitted");
    expect(frame[2] == 1,
           "setting LCDC.1 restores object output on later pixels");
}

void testObjectCanBeEnabledAfterScanlinePreparation() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    writeTileRow(ppu, 1, 0, 0xFF, 0x00); // Object color 1.
    ppu.write(OAM, 16);
    ppu.write(OAM + 1, 8); // Object begins at screen x=0.
    ppu.write(OAM + 2, 1);
    ppu.write(OAM + 3, 0);
    ppu.write(OBP0, 0xE4);
    ppu.write(LCDC, 0x91); // Prepare the scanline with objects hidden.

    ppu.advanceTCycles(78); // Dot 80, after scanline preparation.
    ppu.write(LCDC, 0x93); // Enable objects during Mode 3.
    ppu.advanceTCycles(14);

    expect(ppu.getFrameBuffer()[0] == 1,
           "enabling LCDC.1 during Mode 3 reveals a previously scanned object");
}

void testDisabledObjectsDoNotAddMode3FetchPenalty() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    ppu.write(OAM, 16);
    ppu.write(OAM + 1, 11); // An object that would add a fetch stall.
    ppu.write(LCDC, 0x91); // Keep object output disabled for this scanline.

    ppu.advanceTCycles(249);
    expect((ppu.read(STAT) & 3) == 3,
           "the object enable bit does not expose or charge a hidden object stall");
    ppu.advanceTCycles(1);
    expect((ppu.read(STAT) & 3) == 0,
           "Mode 3 still ends at its baseline when objects are disabled");
}

void testBackgroundDisableAllowsObjectsBehindBackground() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    writeTileRow(ppu, 0, 0, 0xFF, 0x00); // Opaque BG color 1.
    writeTileRow(ppu, 1, 0, 0x00, 0xFF); // Object color 2.
    ppu.write(0x9800, 0);
    ppu.write(OAM, 16);
    ppu.write(OAM + 1, 8); // Object begins at screen x=0.
    ppu.write(OAM + 2, 1);
    ppu.write(OAM + 3, 0x80); // Behind BG while the DMG BG is enabled.
    ppu.write(BGP, 0xE4);
    ppu.write(OBP0, 0xE4);
    ppu.write(LCDC, 0x93);
    ppu.advanceTCycles(100); // Dot 102, during the first object's fetch stall.
    ppu.write(LCDC, 0x92); // Turn off BG before the object pixel is emitted.
    ppu.advanceTCycles(1);

    expect(ppu.getFrameBuffer()[0] == 2,
           "DMG object remains visible when LCDC.0 disables the BG priority layer");
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

void testWindowYTriggerAndHiddenRows() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    // Distinct window rows expose both the trigger latch and row counter.
    writeTileRow(ppu, 1, 0, 0xFF, 0x00);
    writeTileRow(ppu, 1, 1, 0x00, 0xFF);
    ppu.write(0x9C00, 1);
    ppu.write(BGP, 0xE4);
    ppu.write(WX, 7);
    ppu.write(WY, 0);
    ppu.write(LCDC, 0xF1);
    ppu.advanceTCycles(454); // Finish startup line 0 exactly.
    expect(ppu.getFrameBuffer()[0] == 1, "WY equality triggers the first window row");
    ppu.write(WY, 100);
    ppu.write(WX, 167); // Hide line 1 without losing the Y condition.
    ppu.advanceTCycles(456);
    expect(ppu.getFrameBuffer()[160] == 0, "off-screen WX hides the window");
    ppu.write(WX, 7);
    ppu.advanceTCycles(456);
    expect(ppu.getFrameBuffer()[320] == 2,
           "WY changes preserve the trigger and hidden lines preserve the row counter");

    ppu.write(WY, 200); // No equality in the following visible frame.
    ppu.advanceTCycles((154 - 3) * 456 + 456);
    expect(ppu.getFrameBuffer()[0] == 0, "VBlank clears the window Y condition");
    ppu.write(LCDC, 0);
    ppu.write(WY, 0);
    ppu.write(LCDC, 0xF1);
    ppu.advanceTCycles(454);
    expect(ppu.getFrameBuffer()[0] == 1, "LCD restart resets the window row counter");
}

void testWindowYRequiresScanlineStartEquality() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    writeTileRow(ppu, 1, 0, 0xFF, 0x00);
    ppu.write(0x9C00, 1);
    ppu.write(BGP, 0xE4);
    ppu.write(WX, 7);
    ppu.write(WY, 100);
    ppu.write(LCDC, 0xF1);
    ppu.advanceTCycles(454 + 20); // Line 1 has already started.
    ppu.write(WY, 1);
    ppu.advanceTCycles(436);
    expect(ppu.getFrameBuffer()[160] == 0,
           "writing WY equal to LY after scanline start does not trigger the window");
    ppu.write(WY, 0);
    ppu.advanceTCycles(456);
    expect(ppu.getFrameBuffer()[320] == 0,
           "moving WY behind LY does not retroactively trigger the window");
    ppu.write(WY, 4); // Arrange equality at the next scanline start.
    ppu.write(LCDC, 0xD1); // DMG Y condition can trigger while window is disabled.
    ppu.advanceTCycles(456);
    ppu.write(LCDC, 0xF1);
    ppu.advanceTCycles(456);
    expect(ppu.getFrameBuffer()[640] == 1,
           "DMG tracks WY equality independently of window enable");
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

void testPartiallyClippedObjectStartsAtVisibleSourcePixel() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);

    // OAM X=1 leaves only source pixel 7 visible at screen X=0.
    writeTileRow(ppu, 1, 0, 0x01, 0x00);
    ppu.write(BGP, 0xE4);
    ppu.write(OBP0, 0xE4);
    ppu.write(OAM, 16);
    ppu.write(OAM + 1, 1);
    ppu.write(OAM + 2, 1);
    ppu.write(OAM + 3, 0);

    renderFirstScanline(ppu, 0x93);

    const auto &frame = ppu.getFrameBuffer();
    expect(frame[0] == 1,
           "left clipping aligns the OBJ FIFO with the visible source pixel");
    expect(frame[1] == 0,
           "clipped source pixels do not shift later object pixels onto screen");
}

void testOffscreenObjectsConsumeTheTenObjectScanlineLimit() {
    InterruptRegisters firstTenInterrupts;
    DmgPpu firstTen(firstTenInterrupts);
    writeTileRow(firstTen, 2, 0, 0x00, 0xFF); // Opaque object color 2.
    firstTen.write(BGP, 0xE4);
    firstTen.write(OBP0, 0xE4);

    // X=168 hides each object horizontally, but OAM scan still selects them.
    for (uint16_t index = 0; index < 10; ++index) {
        const uint16_t entry = static_cast<uint16_t>(OAM + index * 4);
        firstTen.write(entry, 16);
        firstTen.write(static_cast<uint16_t>(entry + 1), 168);
    }
    firstTen.write(OAM + 40, 16);
    firstTen.write(OAM + 41, 8); // The eleventh object would cover screen x=0.
    firstTen.write(OAM + 42, 2);
    renderFirstScanline(firstTen, 0x93);
    expect(firstTen.getFrameBuffer()[0] == 0,
           "ten earlier off-screen objects prevent the eleventh object from being selected");

    InterruptRegisters nineInterrupts;
    DmgPpu nine(nineInterrupts);
    writeTileRow(nine, 2, 0, 0x00, 0xFF);
    nine.write(BGP, 0xE4);
    nine.write(OBP0, 0xE4);
    for (uint16_t index = 0; index < 10; ++index) {
        const uint16_t entry = static_cast<uint16_t>(OAM + index * 4);
        nine.write(entry, 16);
        nine.write(static_cast<uint16_t>(entry + 1), 168);
    }
    nine.write(OAM, 0); // One earlier object is outside the scanline vertically.
    nine.write(OAM + 40, 16);
    nine.write(OAM + 41, 8);
    nine.write(OAM + 42, 2);
    renderFirstScanline(nine, 0x93);
    expect(nine.getFrameBuffer()[0] == 2,
           "an off-screen Y object does not consume a slot, so the next object is drawn");
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
    testObjectAtXZeroMode0Boundary();
    testObjectsAtSameXRunSeparateFetchEvents();
    testTenObjectsAtXTenMatchMode0TimingBoundary();
    testVblankInterruptAndFrameRollover();
    testLastVblankLineResetsLyBeforeModeEnds();
    testLastVblankLineLycComparisonAndInterruptEdges();
    testModeOneStatBlocksLastLineLycZeroEdge();
    testStatInterruptUsesRisingEdgeOfCombinedSources();
    testModeTwoStatSourceAtVblankEntry();
    testLycCoincidenceAndStatInterrupt();
    testLycComparisonStopsWhileLcdIsOff();
    testBackgroundTilesAndPaletteRenderToFrameBuffer();
    testBackgroundFifoPreservesPixelsAcrossTileMapWrap();
    testBackgroundPaletteWriteAffectsOnlyFuturePixels();
    testSignedBackgroundTileAddressing();
    testWindowPositionAndWindowTileMap();
    testCoarseScrollSamplesLaterTilesWithoutChangingFineDiscard();
    testCoarseScrollWrittenDuringObjectFetchAffectsLaterTiles();
    testObjectFetchStopsBackgroundCatchUpWhenBgFifoHasPixels();
    testObjectBehindBackgroundUsesFetchedScrollPixels();
    testBackgroundEnableIsSampledForEachOutputPixel();
    testBackgroundCanBeEnabledMidScanlineAfterStartingDisabled();
    testObjectEnableIsSampledForEachOutputPixel();
    testObjectCanBeEnabledAfterScanlinePreparation();
    testDisabledObjectsDoNotAddMode3FetchPenalty();
    testBackgroundDisableAllowsObjectsBehindBackground();
    testBackgroundBitplanesSampleScySeparately();
    testScyTileNumberRemainsLatchedBetweenFetchStages();
    testBackgroundTileMapSelectIsSampledAtEachTileFetch();
    testWindowYTriggerAndHiddenRows();
    testWindowYRequiresScanlineStartEquality();
    testObjectsUseTransparencyPaletteAndPriority();
    testPartiallyClippedObjectStartsAtVisibleSourcePixel();
    testOffscreenObjectsConsumeTheTenObjectScanlineLimit();
    testSixteenPixelObjectsUsePairedTiles();
    std::cout << "DMG PPU tests passed\n";
}
