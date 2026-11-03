#include <cstdlib>
#include <array>
#include <iostream>
#include "cgb_hardware.hpp"
#include "interrupt_registers.hpp"
#include "memory_region.hpp"
#include "dmg_serial.hpp"
#include "dmg_oam_dma.hpp"
#include "timer.hpp"
namespace {
using namespace LOTUSGB;
void expect(bool ok,const char *msg) { if(!ok) { std::cerr<<"FAIL: "<<msg<<'\n'; std::exit(1); } }
struct ObservedVideoRam final : IVideoRam {
    CgbVideoRam storage;
    mutable unsigned fetches = 0;
    uint8_t read(uint16_t address) override { return storage.read(address); }
    void write(uint16_t address, uint8_t value) override { storage.write(address, value); }
    uint8_t readBank(uint8_t bank, uint16_t address) const override {
        ++fetches; return storage.readBank(bank, address);
    }
};
class IdleCpu final : public ICPU {
public:
    void reset() override {}
    void fetchFirstOpcode() override {}
    RetStepOneCycle stepOneCycle() override { return {}; }
    Reg getReg() override { return {}; }
    void setReg(const Reg &) override {}
    bool getIME() override { return false; }
    void setIME(bool) override {}
    bool getHALT() override { return false; }
    void setHALT(bool) override {}
    bool getStopped() override { return false; }
    void wakeFromStop() override {}
};
void testSpeedSwitchPpuPermissions() {
    for (uint8_t mode = 0; mode < 4; ++mode) {
        InterruptRegisters irq; ObservedVideoRam video; DmgPpu ppu(irq, &video, true);
        // Tile 0: red BG (index 1); tile 1: green OBJ (index 2).
        for (unsigned row = 0; row < 8; ++row) {
            video.write(0x8000 + row * 2, 0xFF);
            video.write(0x8011 + row * 2, 0xFF);
        }
        ppu.write(0xFF68, 0x82); ppu.write(0xFF69, 0x1F); ppu.write(0xFF69, 0);
        ppu.write(0xFF6A, 0x84); ppu.write(0xFF6B, 0xE0); ppu.write(0xFF6B, 3);
        ppu.write(0xFE00, 16); ppu.write(0xFE01, 8); ppu.write(0xFE02, 1);
        ppu.write(0xFF40, 0x93);
        unsigned dots = 0;
        while ((ppu.read(0xFF41) & 3) != mode && dots++ < 70224) ppu.advanceTCycles(1);
        expect(dots < 70224, "PPU reaches each speed-switch starting mode");
        ppu.beginSpeedSwitchPause(); video.fetches = 0;
        ppu.advanceTCycles(2 * 70224);
        const auto &frame = ppu.getCompletedColorFrameBuffer();
        expect(frame[160 + 16] == (mode < 2 ? 0 : 0x001F),
               "paused H/VBlank renders black; OAM/rendering modes retain the background");
        expect(frame[160] == (mode < 2 ? 0 : mode == 2 ? 0x001F : 0x03E0),
               "only a rendering-mode pause retains sprites across subsequent lines");
        expect((video.fetches != 0) == (mode >= 2), "frozen VRAM lock gates actual PPU bank reads");
        ppu.endSpeedSwitchPause(); ppu.advanceTCycles(2 * 70224);
        expect(ppu.getCompletedColorFrameBuffer()[160] == 0x03E0 &&
                   ppu.getCompletedColorFrameBuffer()[176] == 0x001F,
               "ending a pause restores sprite and background rendering");
    }
    InterruptRegisters irq; DmgPpu dmg(irq);
    try { dmg.beginSpeedSwitchPause(); expect(false, "DMG cannot enter a CGB speed-switch pause"); }
    catch (const std::logic_error &) {}
}

void testPalettePorts() {
    InterruptRegisters irq;
    CgbVideoRam video;
    DmgPpu ppu(irq,&video,true);
    ppu.write(0xFF68,0xBF); ppu.write(0xFF69,0x12);
    expect(ppu.read(0xFF68)==0xC0,"palette auto-increment wraps 63 to 0 and masks reserved bit");
    ppu.write(0xFF6A,0xBF); ppu.write(0xFF6B,0x34);
    ppu.write(0xFF68,0x3F); ppu.write(0xFF6A,0x3F);
    expect(ppu.read(0xFF69)==0x12 && ppu.read(0xFF6B)==0x34,"BG and OBJ palette storage are independent");
    expect(ppu.read(0xFF68)==0x7F,"palette reads do not advance the index");
    ppu.write(0xFF40,0x91);
    for(unsigned i=0;i<456 && (ppu.read(0xFF41)&3)!=3;++i) ppu.advanceTCycles(1);
    expect((ppu.read(0xFF41)&3)==3,"palette test reaches Mode 3");
    ppu.write(0xFF68,0xBF); ppu.write(0xFF69,0x99);
    ppu.write(0xFF6A,0xBF); ppu.write(0xFF6B,0x99);
    expect(ppu.read(0xFF68)==0xC0 && ppu.read(0xFF6A)==0xC0,"blocked palette writes still increment both indices");
    expect(ppu.read(0xFF69)==0xFF && ppu.read(0xFF6B)==0xFF,"Mode 3 blocks palette reads");
    ppu.write(0xFF40,0); ppu.write(0xFF68,0x3F); ppu.write(0xFF6A,0x3F);
    expect(ppu.read(0xFF69)==0x12 && ppu.read(0xFF6B)==0x34,"blocked palette writes leave bytes unchanged");
}
void testNativeCgbWindowDisableResetsYCondition() {
    const auto renderWindowDisable = [](bool cgb, bool compatibility) {
        InterruptRegisters irq;
        CgbVideoRam video;
        DmgPpu ppu(irq, cgb ? &video : nullptr, cgb);
        if (compatibility) ppu.setDmgCompatibility(true, true);

        for (unsigned row = 0; row < 8; ++row)
            ppu.write(0x8010 + row * 2, 0xFF); // Window tile 1 is color index 1.
        ppu.write(0x9C00, 1);
        ppu.write(0xFF47, 0xE4);
        ppu.write(0xFF4A, 0);
        ppu.write(0xFF4B, 7);
        ppu.write(0xFF40, 0xF1); // LCD, BG and window on; separate window map.
        ppu.advanceTCycles(454); // Finish line 0; WY=0 has latched the Y condition.
        const uint8_t firstLine = ppu.getFrameBuffer()[0];

        ppu.write(0xFF40, 0xD1); // Clear LCDC.5 for one write.
        ppu.write(0xFF40, 0xF1);
        ppu.write(0xFF4A, 2); // Re-arm at a future scanline, if the model permits it.
        ppu.advanceTCycles(456);
        const uint8_t afterDisable = ppu.getFrameBuffer()[160];
        ppu.advanceTCycles(456);
        const uint8_t afterFutureWy = ppu.getFrameBuffer()[320];
        return std::array<uint8_t, 3>{firstLine, afterDisable, afterFutureWy};
    };

    const auto nativeCgb = renderWindowDisable(true, false);
    expect(nativeCgb[0] == 1 && nativeCgb[1] == 0 && nativeCgb[2] == 1,
           "native CGB clears the Window Y condition when LCDC.5 is cleared, then relatches at WY");
    const auto dmg = renderWindowDisable(false, false);
    expect(dmg[0] == 1 && dmg[1] == 1,
           "DMG retains the Window Y condition after LCDC.5 is cleared");
    const auto compatibility = renderWindowDisable(true, true);
    expect(compatibility[0] == 1 && compatibility[1] == 1,
           "CGB DMG compatibility retains DMG Window Y-condition behavior");
}
void testLcdcBitZeroDiffersBetweenNativeAndCompatibilityModes() {
    const auto render = [](bool compatibility, bool bgPriorityEnabled) {
        InterruptRegisters irq;
        CgbVideoRam video;
        DmgPpu ppu(irq, &video, true);

        // BG tile 0 is color 1; OBJ tile 1 is color 2.
        for (unsigned row = 0; row < 8; ++row) {
            video.write(static_cast<uint16_t>(0x8000 + row * 2), 0xFF);
            video.write(static_cast<uint16_t>(0x8001 + row * 2), 0x00);
            video.write(static_cast<uint16_t>(0x8010 + row * 2), 0x00);
            video.write(static_cast<uint16_t>(0x8011 + row * 2), 0xFF);
        }
        // Native CGB colors: white/color 1 red for BG, color 2 green for OBJ.
        ppu.write(0xFF68, 0x80);
        for (uint16_t color : {0x7FFF, 0x001F, 0x0000, 0x0000}) {
            ppu.write(0xFF69, static_cast<uint8_t>(color));
            ppu.write(0xFF69, static_cast<uint8_t>(color >> 8));
        }
        ppu.write(0xFF6A, 0x80);
        for (uint16_t color : {0x7FFF, 0x001F, 0x03E0, 0x0000}) {
            ppu.write(0xFF6B, static_cast<uint8_t>(color));
            ppu.write(0xFF6B, static_cast<uint8_t>(color >> 8));
        }
        ppu.write(0xFF47, 0xE4);
        ppu.write(0xFF48, 0xE4);
        ppu.write(0xFE00, 16); // OBJ begins at screen row 0.
        ppu.write(0xFE01, 8);  // OBJ begins at screen column 0.
        ppu.write(0xFE02, 1);
        ppu.write(0xFE03, 0x80); // Behind BG when DMG-style priority applies.
        if (compatibility) ppu.setDmgCompatibility(true, true);

        bool frameCompleted = false;
        ppu.setFrameCompletedCallback([&] { frameCompleted = true; });
        ppu.write(0xFF40, bgPriorityEnabled ? 0x93 : 0x92);
        for (unsigned dot = 0; dot < 70224 && !frameCompleted; ++dot)
            ppu.advanceTCycles(1);
        expect(frameCompleted, "LCDC.0 mode test completes a visible frame");
        const auto &frame = ppu.getCompletedColorFrameBuffer();
        return std::array<uint16_t, 2>{frame[0], frame[8]};
    };

    const auto nativePriorityOff = render(false, false);
    const auto nativePriorityOn = render(false, true);
    expect(nativePriorityOff[0] == 0x03E0 && nativePriorityOff[1] == 0x001F,
           "native CGB keeps BG visible but lets OBJ win when LCDC.0 is clear");
    expect(nativePriorityOn[0] == 0x001F && nativePriorityOn[1] == 0x001F,
           "native CGB applies the OBJ-behind-BG attribute when LCDC.0 is set");

    const auto compatibilityBgOff = render(true, false);
    const auto compatibilityBgOn = render(true, true);
    expect(compatibilityBgOff[0] == 0x03E0 && compatibilityBgOff[1] == 0x7FFF,
           "CGB DMG compatibility disables BG output when LCDC.0 is clear");
    expect(compatibilityBgOn[0] == 0x001F && compatibilityBgOn[1] == 0x001F,
           "CGB DMG compatibility restores DMG-style BG display and OBJ priority");
}
uint8_t renderObjectDuringOamDma(bool cgb, bool startDma) {
    InterruptRegisters irq;
    CgbVideoRam video;
    DmgPpu ppu(irq, cgb ? &video : nullptr, cgb);
    AddressBus bus;
    RamRegion source(0xC000, 0x2000);
    DmgOamDma dma(bus, ppu, cgb);
    bus.map(0xC000, 0xDFFF, source);
    ppu.setOamDmaActiveCallback([&] { return dma.isActive(); });
    ppu.setOamDmaPpuReadAddressCallback(
        [&](size_t address) { return dma.getPpuReadAddress(address); });

    for (unsigned row = 0; row < 8; ++row)
        ppu.write(0x8010 + row * 2, 0xFF); // Object tile 1 has color index 1.
    ppu.write(0xFE00, 16);
    ppu.write(0xFE01, 8);
    ppu.write(0xFE02, 1);
    ppu.write(0xFF47, 0xE4);
    ppu.write(0xFF48, 0xE4);
    ppu.write(0xFF40, 0x93);
    source.write(0xC000, 16);
    source.write(0xC001, 8);
    source.write(0xC002, 1);
    source.write(0xC003, 0);

    ppu.advanceTCycles(454); // Line 1 begins after the startup scanline.
    if (startDma) dma.write(0xFF46, 0xC0);
    for (unsigned dot = 0; dot < 456; ++dot) {
        ppu.advanceTCycles(1);
        dma.advanceTCycles(1);
    }
    return ppu.getFrameBuffer()[160];
}
void testOamDmaHidesObjectsSelectedDuringMode2() {
    for (bool cgb : {false, true}) {
        expect(renderObjectDuringOamDma(cgb, false) == 1,
               "the control scanline renders its selected object without DMA");
        expect(renderObjectDuringOamDma(cgb, true) == 0,
               "active OAM DMA makes Mode-2 selected objects off-screen for that line");
    }
}
uint8_t renderObjectDuringMode3OamDma(bool cgb, bool startDma) {
    InterruptRegisters irq;
    CgbVideoRam video;
    DmgPpu ppu(irq, cgb ? &video : nullptr, cgb);
    AddressBus bus;
    RamRegion source(0xC000, 0x2000);
    DmgOamDma dma(bus, ppu, cgb);
    bus.map(0xC000, 0xDFFF, source);
    ppu.setOamDmaActiveCallback([&] { return dma.isActive(); });
    ppu.setOamDmaPpuReadAddressCallback(
        [&](size_t address) { return dma.getPpuReadAddress(address); });

    for (unsigned row = 0; row < 8; ++row) {
        ppu.write(0x8010 + row * 2, 0xFF); // Tile 1 is OBJ color 1.
        ppu.write(0x8011 + row * 2, 0x00);
        ppu.write(0x8020 + row * 2, 0x00);
        ppu.write(0x8021 + row * 2, 0xFF); // Tile 2 is OBJ color 2.
    }
    ppu.write(0xFE00, 16);
    ppu.write(0xFE01, 88);
    ppu.write(0xFE02, 1);
    ppu.write(0xFF47, 0xE4);
    ppu.write(0xFF48, 0xE4);
    ppu.write(0xFF40, 0x93);
    for (size_t offset = 0; offset < DmgPpu::OAM_SIZE; ++offset)
        source.write(static_cast<uint16_t>(0xC000 + offset),
                     offset % 2 == 0 ? 2 : 0);

    ppu.advanceTCycles(454); // Line 1 begins after the startup scanline.
    unsigned waitDots = 0;
    while ((ppu.read(0xFF41) & 3) != 3 && waitDots++ < 456)
        ppu.advanceTCycles(1);
    expect((ppu.read(0xFF41) & 3) == 3,
           "Mode-3 DMA fixture reaches the visible object-fetch phase");
    if (startDma) dma.write(0xFF46, 0xC0);
    for (unsigned dot = 0; dot < 456; ++dot) {
        ppu.advanceTCycles(1);
        dma.advanceTCycles(1);
    }
    return ppu.getFrameBuffer()[160 + 80];
}
void testOamDmaWordIsSampledDuringMode3ObjectFetch() {
    for (bool cgb : {false, true}) {
        const auto control = renderObjectDuringMode3OamDma(cgb, false);
        const auto dma = renderObjectDuringMode3OamDma(cgb, true);
        expect(control == 1,
               "the control scanline fetches the selected object's original tile");
        expect(dma == 2,
               "Mode-3 OAM DMA supplies the aligned tile/attribute word at object fetch");
    }
}
void testVramDma() {
    InterruptRegisters irq; CgbVideoRam video; DmgPpu ppu(irq,&video,true);
    AddressBus bus; RamRegion work(0xC000,0x2000); bus.map(0xC000,0xDFFF,work);
    CgbVramDma dma(bus,video,ppu);
    for(uint16_t i=0;i<64;++i) work.write(0xC000+i,static_cast<uint8_t>(i+1));
    video.write(0xFF4F,1);
    dma.write(0xFF51,0xC0); dma.write(0xFF52,0x0F);
    dma.write(0xFF53,0xE0); dma.write(0xFF54,0x1F); dma.write(0xFF55,0);
    expect(dma.blocksCpu() && dma.read(0xFF55)==0,"GDMA blocks CPU and reports remaining block count");
    dma.advanceTCycles(31);
    expect(dma.blocksCpu() && video.readBank(1,0x801F)==0,"GDMA takes 32 base T-cycles per 16 bytes");
    dma.advanceTCycles(1);
    expect(!dma.blocksCpu() && dma.read(0xFF55)==0xFF,"GDMA completion releases CPU");
    for(unsigned i=0;i<16;++i)
        expect(video.readBank(1,0x8010+i)==i+1 && video.readBank(0,0x8010+i)==0,"GDMA masks addresses and copies into selected VRAM bank");
    ppu.write(0xFF40,0x91);
    dma.write(0xFF51,0xC0); dma.write(0xFF52,0x10); dma.write(0xFF53,0); dma.write(0xFF54,0x20);
    dma.write(0xFF55,0x82);
    expect(!dma.blocksCpu() && dma.read(0xFF55)==2,"HBlank DMA waits between blocks");
    dma.advanceTCycles(100); expect(video.readBank(1,0x8020)==0,"HBlank DMA cannot run before HBlank entry");
    dma.onHBlank(); expect(dma.blocksCpu(),"HBlank event starts a CPU-blocking block");
    dma.advanceTCycles(32);
    expect(!dma.blocksCpu() && dma.read(0xFF55)==1 && video.readBank(1,0x802F)==32,"HBlank DMA transfers one block then yields");
    dma.write(0xFF55,0);
    expect(dma.read(0xFF55)==0x81,"HBlank cancellation preserves remaining-count readback");
    dma.onHBlank(); dma.advanceTCycles(64);
    expect(video.readBank(1,0x8030)==0,"cancelled HBlank transfer does not resume on later events");
}

void testVramDmaStopsAtVramDestinationOverflow() {
    InterruptRegisters irq;
    CgbVideoRam video;
    DmgPpu ppu(irq, &video, true);
    AddressBus bus;
    RamRegion work(0xC000, 0x2000);
    bus.map(0xC000, 0xDFFF, work);
    CgbVramDma dma(bus, video, ppu);

    for (unsigned i = 0; i < 32; ++i)
        work.write(static_cast<uint16_t>(0xC000 + i),
                   static_cast<uint8_t>(0x40 + i));
    dma.write(0xFF51, 0xC0);
    dma.write(0xFF52, 0x00);
    dma.write(0xFF53, 0x1F);
    dma.write(0xFF54, 0xF0);
    dma.write(0xFF55, 0x01); // Two blocks starting at the last VRAM block.
    dma.advanceTCycles(32);

    expect(!dma.blocksCpu(),
           "VRAM DMA stops when the destination reaches A000");
    for (unsigned i = 0; i < 16; ++i)
        expect(video.readBank(0, static_cast<uint16_t>(0x9FF0 + i)) ==
                   static_cast<uint8_t>(0x40 + i),
               "VRAM DMA completes the last in-range block before overflow");
    expect(video.readBank(0, 0x8000) == 0,
           "VRAM DMA does not wrap an overflowing destination to 8000");

    dma.advanceTCycles(32);
    expect(video.readBank(0, 0x8000) == 0,
           "stopped overflow transfer does not copy a remaining block later");
}

void testGdmaWritesAreBlockedDuringMode3() {
    InterruptRegisters irq;
    CgbVideoRam video;
    DmgPpu ppu(irq, &video, true);
    AddressBus bus;
    RamRegion work(0xC000, 0x2000);
    bus.map(0xC000, 0xDFFF, work);
    CgbVramDma dma(bus, video, ppu);

    for (unsigned i = 0; i < 32; ++i) {
        work.write(static_cast<uint16_t>(0xC000 + i), static_cast<uint8_t>(0xA0 + i));
        video.write(static_cast<uint16_t>(0x8000 + i), 0x5A);
    }
    ppu.write(0xFF40, 0x91);
    unsigned waitDots = 0;
    while ((ppu.read(0xFF41) & 3) != 3 && waitDots++ < 456)
        ppu.advanceTCycles(1);
    expect((ppu.read(0xFF41) & 3) == 3,
           "GDMA mode-3 fixture reaches CGB pixel transfer");

    const auto startBlock = [&](uint8_t destinationLow) {
        dma.write(0xFF51, 0xC0);
        dma.write(0xFF52, 0x00);
        dma.write(0xFF53, 0x00);
        dma.write(0xFF54, destinationLow);
        dma.write(0xFF55, 0x00);
    };
    startBlock(0x00);
    for (unsigned dot = 0; dot < 32; ++dot) {
        ppu.advanceTCycles(1);
        dma.advanceTCycles(1);
    }
    expect(!dma.blocksCpu() && dma.read(0xFF55) == 0xFF,
           "GDMA consumes its block even when the PPU owns VRAM");
    for (unsigned i = 0; i < 16; ++i)
        expect(video.readBank(0, static_cast<uint16_t>(0x8000 + i)) == 0x5A,
               "GDMA destination writes are ignored while CGB Mode 3 owns VRAM");

    waitDots = 0;
    while ((ppu.read(0xFF41) & 3) == 3 && waitDots++ < 456)
        ppu.advanceTCycles(1);
    expect((ppu.read(0xFF41) & 3) == 0,
           "GDMA control fixture reaches HBlank after Mode 3");
    startBlock(0x20);
    for (unsigned dot = 0; dot < 32; ++dot) {
        ppu.advanceTCycles(1);
        dma.advanceTCycles(1);
    }
    for (unsigned i = 0; i < 16; ++i)
        expect(video.readBank(0, static_cast<uint16_t>(0x8020 + i)) == 0xA0 + i,
               "GDMA destination writes succeed when the PPU releases VRAM");
}
void testVramDmaUsesSelectedWramBank() {
    InterruptRegisters irq;
    CgbVideoRam video;
    DmgPpu ppu(irq, &video, true);
    CgbWorkRam work;
    AddressBus bus;
    bus.map(0xC000, 0xDFFF, work);
    bus.map(0xFF70, 0xFF70, work);
    CgbVramDma dma(bus, video, ppu);

    bus.write(0xFF70, 1);
    for (unsigned i = 0; i < 32; ++i)
        bus.write(static_cast<uint16_t>(0xD000 + i), static_cast<uint8_t>(0x10 + i));
    bus.write(0xFF70, 3);
    for (unsigned i = 0; i < 32; ++i)
        bus.write(static_cast<uint16_t>(0xD000 + i), static_cast<uint8_t>(0xA0 + i));

    auto transferBlock = [&](uint8_t control, uint8_t destinationOffset,
                             const char *label) {
        bus.write(0xFF70, 3);
        dma.write(0xFF51, 0xD0);
        dma.write(0xFF52, 0x00);
        dma.write(0xFF53, 0x00);
        dma.write(0xFF54, destinationOffset);
        dma.write(0xFF55, control);
        dma.advanceTCycles(32);
        for (unsigned i = 0; i < 16; ++i)
            expect(video.readBank(0, static_cast<uint16_t>(
                       0x8000 + destinationOffset + i)) == 0xA0 + i,
                   label);
    };

    transferBlock(0x00, 0x00,
                  "GDMA reads WRAM bank selected by SVBK instead of bank-1 decoys");
    transferBlock(0x80, 0x20,
                  "LCD-off HBlank DMA reads WRAM bank selected by SVBK");
}
void testVramDmaUsesBaseClockAtDoubleSpeed() {
    for (const bool doubleSpeed : {false, true}) {
        for (const bool hblankMode : {false, true}) {
            InterruptRegisters irq;
            CgbVideoRam video;
            DmgPpu ppu(irq, &video, true);
            AddressBus bus;
            RamRegion work(0xC000, 0x2000);
            bus.map(0xC000, 0xDFFF, work);
            CgbVramDma dma(bus, video, ppu);
            for (unsigned i = 0; i < 16; ++i)
                work.write(static_cast<uint16_t>(0xC000 + i), static_cast<uint8_t>(0xA0 + i));

            dma.write(0xFF51, 0xC0);
            dma.write(0xFF52, 0x00);
            dma.write(0xFF53, 0x00);
            dma.write(0xFF54, 0x00);
            dma.write(0xFF55, hblankMode ? 0x80 : 0x00);
            expect(dma.blocksCpu(), "GDMA or LCD-off HBlank DMA starts before clock-rate check");

            IdleCpu cpu;
            SystemClock clock(cpu);
            clock.addClockedComponent(dma);
            clock.setDoubleSpeed(doubleSpeed);
            const unsigned stepsToBoundary = doubleSpeed ? 14 : 7;
            const unsigned cpuTCyclesPerStep = doubleSpeed ? 2 : 4;
            for (unsigned step = 0; step < stepsToBoundary; ++step) {
                const auto elapsed = clock.stepIdle();
                expect(elapsed.success && elapsed.tCyclesElapsed == cpuTCyclesPerStep,
                       "clock idle reports the selected CPU-speed M-cycle duration");
            }
            expect(dma.blocksCpu() && clock.getElapsedTCycles() == 28,
                   "VRAM DMA remains active after 28 base-clock T-cycles at either CPU speed");
            if (doubleSpeed) {
                const auto extraMcycle = clock.stepIdle();
                expect(extraMcycle.success && dma.blocksCpu() &&
                           clock.getElapsedTCycles() == 30,
                       "double-speed VRAM DMA still needs two base clocks for its final byte");
            }
            const auto finalStep = clock.stepIdle();
            expect(finalStep.success && !dma.blocksCpu() && clock.getElapsedTCycles() == 32,
                   "VRAM DMA block completes after 32 base-clock T-cycles at either CPU speed");
            for (unsigned i = 0; i < 16; ++i)
                expect(video.readBank(0, static_cast<uint16_t>(0x8000 + i)) == 0xA0 + i,
                       "fixed-rate VRAM DMA copies every byte at either CPU speed");
        }
    }
}
void testHblankDmaTransfersOneBlockWithLcdOff() {
    InterruptRegisters irq;
    CgbVideoRam video;
    DmgPpu ppu(irq, &video, true);
    AddressBus bus;
    RamRegion work(0xC000, 0x2000);
    bus.map(0xC000, 0xDFFF, work);
    CgbVramDma dma(bus, video, ppu);
    ppu.setHBlankCallback([&] { dma.onHBlank(); });
    ppu.setLcdDisabledCallback([&] { dma.onLcdDisabled(); });
    for (uint16_t i = 0; i < 48; ++i)
        work.write(static_cast<uint16_t>(0xC000 + i), static_cast<uint8_t>(0x40 + i));

    dma.write(0xFF51, 0xC0); dma.write(0xFF52, 0x00);
    dma.write(0xFF53, 0x00); dma.write(0xFF54, 0x40);
    dma.write(0xFF55, 0x82);
    expect(dma.blocksCpu() && dma.read(0xFF55) == 2,
           "LCD-off HBlank DMA starts one block and retains the remaining length");
    dma.advanceTCycles(31);
    expect(video.readBank(0, 0x804F) == 0,
           "LCD-off HBlank DMA retains the eight-M-cycle block duration");
    dma.advanceTCycles(1);
    expect(!dma.blocksCpu() && dma.read(0xFF55) == 1 &&
               video.readBank(0, 0x8040) == 0x40 && video.readBank(0, 0x804F) == 0x4F,
           "LCD-off HBlank DMA yields after copying exactly one 16-byte block");
    dma.advanceTCycles(64);
    expect(dma.read(0xFF55) == 1 && video.readBank(0, 0x8050) == 0,
           "LCD-off HBlank DMA waits instead of consuming later blocks");

    ppu.write(0xFF40, 0x91);
    for (unsigned dot = 0; dot < 300; ++dot) {
        ppu.advanceTCycles(1);
        dma.advanceTCycles(1);
    }
    expect(dma.read(0xFF55) == 0 && video.readBank(0, 0x8050) == 0x50 &&
               video.readBank(0, 0x8060) == 0,
           "resumed LCD operation transfers one retained block at the next HBlank");
    for (unsigned dot = 300; dot < 800; ++dot) {
        ppu.advanceTCycles(1);
        dma.advanceTCycles(1);
    }
    expect(dma.read(0xFF55) == 0xFF && video.readBank(0, 0x8060) == 0x60 &&
               video.readBank(0, 0x806F) == 0x6F,
           "remaining HBlank DMA blocks complete on successive visible HBlanks");
}
void testHblankDmaPausesAcrossVblank() {
    InterruptRegisters irq;
    CgbVideoRam video;
    DmgPpu ppu(irq, &video, true);
    ppu.initializeCgbPostBootState();
    AddressBus bus;
    RamRegion work(0xC000, 0x2000);
    bus.map(0xC000, 0xDFFF, work);
    CgbVramDma dma(bus, video, ppu);
    unsigned hblankEvents = 0;
    ppu.setHBlankCallback([&] { ++hblankEvents; dma.onHBlank(); });
    for (uint16_t i = 0; i < 32; ++i)
        work.write(static_cast<uint16_t>(0xC000 + i), static_cast<uint8_t>(0x40 + i));

    // Enter visible line 143 in Mode 2, then arm two blocks before its HBlank.
    for (unsigned cycle = 0; cycle < 456U * 143U; ++cycle)
        ppu.advanceTCycles(1);
    expect(ppu.read(0xFF44) == 143 && (ppu.read(0xFF41) & 3) == 2 &&
               hblankEvents == 143,
           "HBlank DMA VBlank fixture reaches line 143 before its HBlank");
    dma.write(0xFF51, 0xC0); dma.write(0xFF52, 0x00);
    dma.write(0xFF53, 0x00); dma.write(0xFF54, 0x00);
    dma.write(0xFF55, 0x81);

    for (unsigned cycle = 0; cycle < 456; ++cycle) {
        ppu.advanceTCycles(1);
        dma.advanceTCycles(1);
    }
    expect(ppu.read(0xFF44) == 144 && (ppu.read(0xFF41) & 3) == 1 &&
               hblankEvents == 144 && dma.read(0xFF55) == 0 &&
               video.readBank(0, 0x800F) == 0x4F &&
               video.readBank(0, 0x8010) == 0,
           "line 143 HBlank copies one block and leaves one active for VBlank");

    for (unsigned cycle = 0; cycle < 456U * 10U; ++cycle) {
        ppu.advanceTCycles(1);
        dma.advanceTCycles(1);
    }
    expect(ppu.read(0xFF44) == 0 && (ppu.read(0xFF41) & 3) == 2 &&
               hblankEvents == 144 && dma.read(0xFF55) == 0 &&
               video.readBank(0, 0x8010) == 0,
           "HBlank DMA preserves its final block throughout VBlank until line 0");

    for (unsigned cycle = 0; cycle < 456 && dma.read(0xFF55) != 0xFF; ++cycle) {
        ppu.advanceTCycles(1);
        dma.advanceTCycles(1);
    }
    expect(hblankEvents == 145 && dma.read(0xFF55) == 0xFF &&
               video.readBank(0, 0x8010) == 0x50 &&
               video.readBank(0, 0x801F) == 0x5F,
           "HBlank DMA resumes on line 0 and completes its retained block");
}
void testFastSerialAndDoubleSpeedDividerTap() {
    InterruptRegisters irq;
    DmgSerial serial(irq,true);
    serial.write(0xFF01,0); serial.write(0xFF02,0x83);
    serial.advanceTCycles(127);
    expect(serial.read(0xFF02)&0x80,"CGB fast serial remains busy before the eighth bit");
    serial.advanceTCycles(1);
    expect(!(serial.read(0xFF02)&0x80) && serial.read(0xFF01)==0xFF && (irq.read(0xFF0F)&8),
           "CGB fast serial completes eight incoming-one bits in 128 CPU clocks");
    Timer timer(irq); unsigned edges=0;
    timer.setDividerApuClockCallback([&] { ++edges; });
    timer.setDoubleSpeed(true);
    timer.advanceTCycles(8192);
    expect(edges==0,"double-speed DIV uses bit 13 for the APU frame sequencer");
    timer.advanceTCycles(8192);
    expect(edges==1,"double-speed CPU timer preserves the base-rate APU divider cadence");
}

void testDmgStatWriteGlitchIsAbsentOnCgb() {
    for (bool cgb : {false, true}) {
        for (unsigned mode = 0; mode < 4; ++mode) {
            for (bool match : {false, true}) {
                InterruptRegisters irq; CgbVideoRam video; DmgPpu ppu(irq, &video, cgb);
                const uint8_t line = mode == 1 ? 144 : 1;
                ppu.write(0xFF45, match ? line : 200);
                ppu.write(0xFF40, 0x91);
                unsigned dots = 0;
                for (; dots < 70224 && (ppu.read(0xFF44) != line || (ppu.read(0xFF41) & 3) != mode); ++dots)
                    ppu.advanceTCycles(1);
                expect(dots < 70224, "STAT glitch probe reaches requested PPU mode and line");
                irq.write(0xFF0F, 0); ppu.write(0xFF41, 0);
                const bool glitch = !cgb && (mode != 3 || match);
                expect(bool(irq.read(0xFF0F) & 2) == glitch,
                       "DMG zero STAT write triggers outside mode 3 or on LYC match; CGB does not");
                irq.write(0xFF0F, 0); ppu.advanceTCycles(3); ppu.write(0xFF41, 0);
                expect(!(irq.read(0xFF0F) & 2), "one-M-cycle STAT override holds combined line without repeated IRQ");
                ppu.advanceTCycles(4); ppu.write(0xFF41, 0);
                expect(bool(irq.read(0xFF0F) & 2) == glitch,
                       "STAT override releases after four clocks and permits a fresh write edge");
                ppu.advanceTCycles(4);
                expect((ppu.read(0xFF41) & 0x78) == 0, "STAT write glitch does not permanently enable interrupt sources");
                ppu.write(0xFF40, 0); irq.write(0xFF0F, 0); ppu.write(0xFF41, 0);
                expect(!(irq.read(0xFF0F) & 2), "LCD-off STAT writes do not create spurious interrupts");
            }
        }
    }
}

void testOamCorruptionPatternsAndModelGate() {
    using Event = CpuBusEvent;
    struct Case { unsigned row; Event event; uint16_t address; bool mode3 = false; };
    for (bool cgb : {false, true}) {
        for (const auto test : {Case{0, Event::ReadIdu, 0xFE00}, Case{3, Event::ReadIdu, 0xFEFF},
                               Case{4, Event::ReadIdu, 0xFEFF},
                               Case{5, Event::Read, 0xFE00}, Case{5, Event::Write, 0xFEA0},
                               Case{5, Event::Idu, 0xFEFF}, Case{5, Event::WriteIdu, 0xFE80},
                               Case{5, Event::ReadIdu, 0xFE10}, Case{18, Event::ReadIdu, 0xFEFF},
                               Case{19, Event::ReadIdu, 0xFEFF},
                               Case{5, Event::Idu, 0xFDFF}, Case{5, Event::Idu, 0xFF00},
                               Case{5, Event::ReadIdu, 0xFE00, true}}) {
            InterruptRegisters irq; CgbVideoRam video; DmgPpu ppu(irq, &video, cgb);
            std::array<uint8_t, 160> before{};
            for (unsigned i = 0; i < before.size(); ++i) before[i] = static_cast<uint8_t>(i * 37 + 0x51);
            const auto row = [&](auto &bytes, unsigned r, std::array<uint16_t, 4> words) {
                for (unsigned w = 0; w < 4; ++w) {
                    bytes[r * 8 + w * 2] = words[w]; bytes[r * 8 + w * 2 + 1] = words[w] >> 8;
                }
            };
            row(before, 2, {0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD});
            row(before, 3, {0x1357, 0x0011, 0x0022, 0x0033});
            row(before, 4, {0x2468, 0x4455, 0x6789, 0xAABB});
            row(before, 5, {0xFEDC, 0x1234, 0x5678, 0x9ABC});
            row(before, 18, {0xFEDC, 0x1234, 0x5678, 0x9ABC});
            row(before, 19, {0x2468, 0x9876, 0x5432, 0x10FE});
            for (unsigned i = 0; i < before.size(); ++i) ppu.write(0xFE00 + i, before[i]);
            ppu.onCpuBusAccess(0xFE00, Event::Idu); // LCD off must have no effect.
            ppu.write(0xFF40, 0x91);
            ppu.advanceTCycles(454 + (test.mode3 ? 100 : test.row * 4)); // Line 1, requested scan row.
            expect((ppu.read(0xFF41) & 3) == (test.mode3 ? 3 : 2) && ppu.read(0xFF44) == 1, "OAM pattern test reaches mode-two scan row");
            ppu.onCpuBusAccess(test.address, test.event);
            ppu.write(0xFF40, 0);
            auto expected = before;
            if (!cgb && !test.mode3 && (test.address & 0xFF00) == 0xFE00) {
                if (test.row == 3) row(expected, 3, {0xAAEE, 0xBBBB, 0xCCCC, 0xDDDD});
                if (test.row == 19) row(expected, 19, {0xFEFC, 0x1234, 0x5678, 0x9ABC});
                if (test.row == 4) {
                    for (unsigned r : {2u, 3u, 4u})
                        row(expected, r, {0x0262, 0x0011, 0x0022, 0x0033});
                }
                if (test.row == 5) {
                    if (test.event == Event::ReadIdu) {
                        for (unsigned r : {3u, 4u, 5u}) row(expected, r, {0x2648, 0x4455, 0x6789, 0xAABB});
                    } else row(expected, 5, {static_cast<uint16_t>(test.event == Event::Read ? 0x66E8 : 0x66C8), 0x4455, 0x6789, 0xAABB});
                }
                if (test.row == 18) {
                    for (unsigned r : {16u, 17u, 18u})
                        row(expected, r, {0xBED9, 0x6843, 0xB28D, 0xFCD7});
                }
            }
            for (unsigned i = 0; i < before.size(); ++i)
                expect(ppu.read(0xFE00 + i) == expected[i], "OAM read/write/combined patterns match golden words, protected rows, address range and CGB immunity");
        }
    }
}

void testOamCorruptionSelectsEveryMode2RowAtMCycleBoundary() {
    for (unsigned scanRow = 0; scanRow <= 20; ++scanRow) {
        InterruptRegisters irq;
        DmgPpu ppu(irq);
        std::array<uint8_t, DmgPpu::OAM_SIZE> before{};
        for (unsigned i = 0; i < before.size(); ++i) {
            before[i] = static_cast<uint8_t>(i * 37 + 0x51);
            ppu.write(static_cast<uint16_t>(0xFE00 + i), before[i]);
        }

        ppu.write(0xFF40, 0x91);
        // LCD startup ends at LY 1 dot 0. Each subsequent four-dot point is
        // the beginning of the next eight-byte OAM row's M-cycle slot.
        ppu.advanceTCycles(454 + scanRow * 4);
        expect(ppu.read(0xFF44) == 1 &&
                   (ppu.read(0xFF41) & 3) == (scanRow == 20 ? 3 : 2),
               "OAM row probe reaches its M-cycle boundary or Mode 3 end");
        ppu.onCpuBusAccess(0xFEA0, CpuBusEvent::Write);
        ppu.write(0xFF40, 0);

        auto expected = before;
        if (scanRow > 0 && scanRow < 20) {
            const auto word = [&](unsigned row, unsigned index) {
                const unsigned offset = row * 8 + index * 2;
                return static_cast<uint16_t>(before[offset] |
                                             (before[offset + 1] << 8));
            };
            const unsigned row = scanRow;
            const uint16_t a = word(row, 0);
            const uint16_t b = word(row - 1, 0);
            const uint16_t c = word(row - 1, 2);
            const uint16_t corrupted =
                static_cast<uint16_t>(((a ^ c) & (b ^ c)) ^ c);
            const unsigned current = row * 8;
            const unsigned previous = (row - 1) * 8;
            expected[current] = static_cast<uint8_t>(corrupted);
            expected[current + 1] = static_cast<uint8_t>(corrupted >> 8);
            for (unsigned byte = 2; byte < 8; ++byte)
                expected[current + byte] = before[previous + byte];
        }
        for (unsigned i = 0; i < expected.size(); ++i)
            expect(ppu.read(static_cast<uint16_t>(0xFE00 + i)) == expected[i],
                   "OAM corruption affects only the row selected at each M-cycle boundary");
    }
}

void testCgbOamDmaUsesSelectedVramAndWramBanks() {
    InterruptRegisters irq;
    CgbVideoRam video;
    CgbWorkRam work;
    DmgPpu ppu(irq, &video, true);
    AddressBus bus;
    DmgOamDma dma(bus, ppu, true);
    bus.map(0x8000, 0x9FFF, ppu);
    bus.map(0xC000, 0xDFFF, work);
    bus.mapAlias(0xE000, 0xFDFF, work, 0xC000);
    bus.map(0xFE00, 0xFE9F, ppu);
    bus.map(0xFF46, 0xFF46, dma);
    bus.setCpuBusArbiter(dma);

    std::array<uint8_t, DmgPpu::OAM_SIZE> expectedVram{}, expectedWram{};
    for (unsigned i = 0; i < DmgPpu::OAM_SIZE; ++i) {
        expectedVram[i] = static_cast<uint8_t>(0xA0 ^ i);
        expectedWram[i] = static_cast<uint8_t>(0x50 + i);
    }
    video.write(0xFF4F, 1);
    for (unsigned i = 0; i < expectedVram.size(); ++i)
        video.write(static_cast<uint16_t>(0x8000 + i), expectedVram[i]);
    video.write(0xFF4F, 0);
    for (unsigned i = 0; i < expectedVram.size(); ++i)
        video.write(static_cast<uint16_t>(0x8000 + i), static_cast<uint8_t>(0x13 + i));
    video.write(0xFF4F, 1);

    work.write(0xFF70, 3);
    for (unsigned i = 0; i < expectedWram.size(); ++i)
        work.write(static_cast<uint16_t>(0xD000 + i), expectedWram[i]);
    work.write(0xFF70, 1);
    for (unsigned i = 0; i < expectedWram.size(); ++i)
        work.write(static_cast<uint16_t>(0xD000 + i), static_cast<uint8_t>(0x20 + i));
    work.write(0xFF70, 3);

    const auto checkDma = [&](uint8_t sourcePage, const auto &expected,
                              const char *message) {
        dma.write(0xFF46, sourcePage);
        dma.advanceTCycles(648);
        for (unsigned i = 0; i < expected.size(); ++i)
            expect(ppu.read(static_cast<uint16_t>(0xFE00 + i)) == expected[i], message);
    };
    checkDma(0x80, expectedVram, "CGB OAM DMA reads the selected VBK source bank");
    checkDma(0xD0, expectedWram, "CGB OAM DMA reads the selected SVBK source bank");
}

void testCgbOamDmaReadsCartridgeRomSource() {
    InterruptRegisters irq;
    CgbVideoRam video;
    DmgPpu ppu(irq, &video, true);
    AddressBus bus;
    RamRegion cartridgeRom(0x0000, 0x8000);
    DmgOamDma dma(bus, ppu, true);
    bus.map(0x0000, 0x7FFF, cartridgeRom);
    bus.map(0xFE00, 0xFE9F, ppu);
    bus.map(0xFF46, 0xFF46, dma);
    bus.setCpuBusArbiter(dma);

    std::array<uint8_t, DmgPpu::OAM_SIZE> expected{};
    for (unsigned i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<uint8_t>(0x81 + i * 3);
        cartridgeRom.write(static_cast<uint16_t>(0x4000 + i), expected[i]);
    }

    dma.write(0xFF46, 0x40);
    dma.advanceTCycles(648);
    for (unsigned i = 0; i < expected.size(); ++i)
        expect(ppu.read(static_cast<uint16_t>(0xFE00 + i)) == expected[i],
               "CGB OAM DMA copies its cartridge-ROM window source page");
}

}
int main() { testSpeedSwitchPpuPermissions(); testOamCorruptionPatternsAndModelGate(); testOamCorruptionSelectsEveryMode2RowAtMCycleBoundary(); testDmgStatWriteGlitchIsAbsentOnCgb(); testPalettePorts(); testNativeCgbWindowDisableResetsYCondition(); testLcdcBitZeroDiffersBetweenNativeAndCompatibilityModes(); testOamDmaHidesObjectsSelectedDuringMode2(); testOamDmaWordIsSampledDuringMode3ObjectFetch(); testCgbOamDmaUsesSelectedVramAndWramBanks(); testCgbOamDmaReadsCartridgeRomSource(); testVramDma(); testVramDmaStopsAtVramDestinationOverflow(); testGdmaWritesAreBlockedDuringMode3(); testVramDmaUsesSelectedWramBank(); testVramDmaUsesBaseClockAtDoubleSpeed(); testHblankDmaTransfersOneBlockWithLcdOff(); testHblankDmaPausesAcrossVblank(); testFastSerialAndDoubleSpeedDividerTap(); std::cout<<"CGB palette-port and VRAM-DMA tests passed\n"; }
