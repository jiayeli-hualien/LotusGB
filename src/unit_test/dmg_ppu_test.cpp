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

constexpr uint8_t STAT_MODE0_INTERRUPT = 0x08;
constexpr uint8_t STAT_MODE2_INTERRUPT = 0x20;
constexpr uint8_t STAT_LYC_INTERRUPT = 0x40;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
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

}

int main() {
    testDisabledPpuRegistersAndVideoMemory();
    testVisibleLineModesAndVideoMemoryAccess();
    testVblankInterruptAndFrameRollover();
    testStatInterruptUsesRisingEdgeOfCombinedSources();
    testLycCoincidenceAndStatInterrupt();
    std::cout << "DMG PPU tests passed\n";
}
