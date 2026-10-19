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
    ppu.advanceTCycles(252);
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
    expect((ppu.read(STAT) & 0x03) == 2, "LCD enable begins visible line in mode 2");
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

    ppu.advanceTCycles(203);
    expect(ppu.read(LY) == 0, "LY stays on the current line before dot 456");
    ppu.advanceTCycles(1);
    expect(ppu.read(LY) == 1, "LY increments after 456 dots");
    expect((ppu.read(STAT) & 0x03) == 2, "the next visible line begins in mode 2");
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
           "entering enabled mode 2 requests a STAT interrupt");

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
    testVblankInterruptAndFrameRollover();
    testStatInterruptUsesRisingEdgeOfCombinedSources();
    testLycCoincidenceAndStatInterrupt();
    testBackgroundTilesAndPaletteRenderToFrameBuffer();
    testSignedBackgroundTileAddressing();
    testWindowPositionAndWindowTileMap();
    testObjectsUseTransparencyPaletteAndPriority();
    testSixteenPixelObjectsUsePairedTiles();
    std::cout << "DMG PPU tests passed\n";
}
