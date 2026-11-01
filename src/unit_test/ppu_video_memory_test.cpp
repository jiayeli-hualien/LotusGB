#include <cstdlib>
#include <iostream>
#include <stdexcept>

#include "cgb_memory.hpp"
#include "dmg_ppu.hpp"
#include "interrupt_registers.hpp"

namespace {
using namespace LOTUSGB;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// Original monochrome scene: patterned BG, a window, and opaque/transparent
// objects. Run the same renderer with owned and external VRAM; this does not
// exercise CGB attributes, palettes or CGB timing.
void initializeScene(DmgPpu &ppu) {
    ppu.write(0xFF47, 0xE4);
    ppu.write(0xFF48, 0xE4);
    ppu.write(0xFF49, 0x1B);
    for (uint8_t row = 0; row < 8; ++row) {
        ppu.write(0x8000 + row * 2, 0xAA);
        ppu.write(0x8001 + row * 2, 0xCC); // BG: 3,2,1,0 repeating
        ppu.write(0x8010 + row * 2, 0x00);
        ppu.write(0x8011 + row * 2, 0xFF); // Window: 2
        ppu.write(0x8020 + row * 2, 0xFF);
        ppu.write(0x8021 + row * 2, 0x00); // OBJ: 1
        ppu.write(0x8030 + row * 2, 0xA5);
        ppu.write(0x8031 + row * 2, 0x00); // OBJ with transparent pixels
    }
    for (uint16_t address = 0x9800; address < 0x9C00; ++address)
        ppu.write(address, 0);
    for (uint16_t address = 0x9C00; address <= 0x9FFF; ++address)
        ppu.write(address, 1);
    const uint8_t objects[] = {
        16, 8, 2, 0,      // (0,0), opaque
        20, 40, 3, 0x30, // flipped X, palette 1
        24, 56, 3, 0x80, // behind nonzero BG
    };
    for (size_t offset = 0; offset < sizeof(objects); ++offset)
        ppu.write(static_cast<uint16_t>(0xFE00 + offset), objects[offset]);
    ppu.write(0xFF4A, 8);
    ppu.write(0xFF4B, 87);
    ppu.write(0xFF41, 0x78); // All STAT sources
    ppu.write(0xFF45, 8);
}

void testRenderedSceneIgnoresCpuBankAndSharesLiveStorage() {
    InterruptRegisters ownedIrq, externalIrq;
    CgbVideoRam video;
    DmgPpu owned(ownedIrq), external(externalIrq, &video);
    initializeScene(owned);
    initializeScene(external);
    video.write(0xFF4F, 1);
    for (uint16_t address = 0x8000; address <= 0x9FFF; ++address)
        external.write(address, 0xFF); // Contradict bank-0 tile/map data
    expect(external.read(0x8000) == 0xFF && video.readBank(0, 0x8000) == 0xAA,
           "CPU bank-1 view and PPU bank-0 data remain independent");

    unsigned ownedFrames = 0, externalFrames = 0;
    owned.setFrameCompletedCallback([&] { ++ownedFrames; });
    external.setFrameCompletedCallback([&] { ++externalFrames; });
    owned.write(0xFF40, 0xF3); // BG, unsigned tiles, window map 1, OBJ, LCD
    external.write(0xFF40, 0xF3);
    for (unsigned frame = 0; frame < 2; ++frame) {
        for (unsigned dot = 0; dot < 70224; ++dot) {
            owned.advanceTCycles(1);
            external.advanceTCycles(1);
            expect(owned.read(0xFF41) == external.read(0xFF41)
                   && owned.read(0xFF44) == external.read(0xFF44)
                   && ownedIrq.read(0xFF0F) == externalIrq.read(0xFF0F),
                   "owned/external storage preserves per-dot mode, LY and interrupt behavior");
            if (dot % 456 == 455)
                expect(owned.getFrameBuffer() == external.getFrameBuffer(),
                       "BG, window and OBJ pixels match across video storage implementations");
        }
        expect(ownedFrames == frame + 1 && externalFrames == ownedFrames,
               "storage selection preserves actual frame completion cadence");
        expect(owned.getCompletedFrameBuffer() == external.getCompletedFrameBuffer(),
               "complete frames match with CPU VBK left at bank 1");
        const auto &pixels = external.getCompletedFrameBuffer();
        expect(pixels[0] == 1 && pixels[8] == 3 && pixels[9] == 2
               && pixels[10] == 1 && pixels[11] == 0
               && pixels[8 * 160 + 80] == (frame == 0 ? 2 : 3),
               "external VRAM actually supplies distinct expected OBJ/BG/window pixels");
        if (frame == 0) {
            // Change bank 0 through the storage owner before the next visible
            // fetches, keeping CPU VBK at 1. A shadow VRAM would miss it.
            video.write(0xFF4F, 0);
            for (uint8_t row = 0; row < 8; ++row) {
                owned.write(0x8010 + row * 2, 0xFF);
                video.write(0x8010 + row * 2, 0xFF); // Window becomes color 3
            }
            video.write(0xFF4F, 1);
        }
    }
    expect(external.getCompletedFrameBuffer()[8 * 160 + 80] == 3,
           "PPU fetches observe live owner writes without a VRAM copy or VBK change");
    expect(video.read(0xFF4F) == 0xFF, "PPU fetches never change CPU VBK");
}

void testCpuAccessWindowsApplyBeforeSelectedBankStorage() {
    InterruptRegisters irq;
    CgbVideoRam video;
    DmgPpu ppu(irq, &video);
    AddressBus bus;
    bus.map(0x8000, 0x9FFF, ppu);
    bus.map(0xFF4F, 0xFF4F, video);
    for (uint8_t bank = 0; bank < 2; ++bank) {
        bus.write(0xFF4F, bank);
        bus.write(0x8000, static_cast<uint8_t>(0x11 + bank));
        expect(bus.read(0x8000) == 0x11 + bank,
               "LCD-off CPU accesses reach the selected external bank through the PPU");
    }
    ppu.write(0xFF40, 0x91);
    unsigned elapsed = 0;
    while ((ppu.read(0xFF41) & 3) != 3 && elapsed++ < 456)
        ppu.advanceTCycles(1);
    expect((ppu.read(0xFF41) & 3) == 3, "probe reaches active pixel transfer");
    for (uint8_t bank = 0; bank < 2; ++bank) {
        bus.write(0xFF4F, bank);
        expect(bus.read(0x8000) == 0xFF, "Mode 3 CPU reads are blocked in either bank");
        bus.write(0x8000, 0x99);
        expect(video.readBank(bank, 0x8000) == 0x11 + bank,
               "Mode 3 CPU writes are discarded before reaching bank storage");
    }
    while ((ppu.read(0xFF41) & 3) != 0 && elapsed++ < 456)
        ppu.advanceTCycles(1);
    expect((ppu.read(0xFF41) & 3) == 0, "probe reaches HBlank");
    for (uint8_t bank = 0; bank < 2; ++bank) {
        bus.write(0xFF4F, bank);
        expect(bus.read(0x8000) == 0x11 + bank, "blocked writes are not replayed in HBlank");
        bus.write(0x8000, static_cast<uint8_t>(0x33 + bank));
        expect(video.readBank(bank, 0x8000) == 0x33 + bank,
               "HBlank CPU writes reach the shared selected bank");
    }
}
}

int main() {
    testRenderedSceneIgnoresCpuBankAndSharesLiveStorage();
    testCpuAccessWindowsApplyBeforeSelectedBankStorage();
    std::cout << "PPU shared VRAM, bank independence and access-window tests passed\n";
}
