#include <cstdlib>
#include <iostream>

#include "dmg_oam_dma.hpp"
#include "cgb_memory.hpp"
#include "interrupt_registers.hpp"
#include "memory_region.hpp"
#include "system_clock.hpp"

namespace {

using namespace LOTUSGB;

class IdleCpu final : public ICPU {
public:
    void reset() override {}
    void fetchFirstOpcode() override {}
    RetStepOneCycle stepOneCycle() override { return {true, false, T_CYCLES_PER_M_CYCLE}; }
    Reg getReg() override { return {}; }
    void setReg(const Reg &) override {}
    bool getIME() override { return false; }
    void setIME(bool) override {}
    bool getHALT() override { return false; }
    void setHALT(bool) override {}
    bool getStopped() override { return false; }
    void wakeFromStop() override {}
};

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void testDmgOamDmaCopies160BytesFromSelectedPage() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    AddressBus bus;
    RamRegion workRam(0xC000, 0x2000);
    DmgOamDma dma(bus, ppu);
    bus.map(0xC000, 0xDFFF, workRam);
    bus.map(0xFF46, 0xFF46, dma);

    for (size_t i = 0; i < DmgPpu::OAM_SIZE; ++i)
        workRam.write(static_cast<uint16_t>(0xC000 + i),
                      static_cast<uint8_t>(i ^ 0x5A));

    bus.write(0xFF46, 0xC0);
    dma.advanceTCycles(648);
    expect(bus.read(0xFF46) == 0xC0,
           "DMA register retains the selected source page");
    for (size_t i = 0; i < DmgPpu::OAM_SIZE; ++i)
        expect(ppu.read(static_cast<uint16_t>(0xFE00 + i)) ==
                   static_cast<uint8_t>(i ^ 0x5A),
               "DMA copies every source byte into OAM");
}

void testDmgOamDmaHighSourcePagesAliasWorkRam() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    AddressBus bus;
    RamRegion workRam(0xC000, 0x2000);
    RamRegion io(0xFF00, 0x46);
    DmgOamDma dma(bus, ppu);
    bus.map(0xC000, 0xDFFF, workRam);
    bus.mapAlias(0xE000, 0xFDFF, workRam, 0xC000);
    bus.map(0xFE00, 0xFE9F, ppu);
    bus.map(0xFF00, 0xFF45, io);
    bus.map(0xFF46, 0xFF46, dma);
    workRam.write(0xDE00, 0x3C);
    workRam.write(0xDF00, 0xA5);
    ppu.write(0xFE00, 0x11);
    io.write(0xFF00, 0x22);
    expect(bus.read(0xFE00) == 0x11 && bus.read(0xFF00) == 0x22,
           "CPU reads retain ordinary OAM and I/O mappings");

    for (unsigned page = 0xE0; page <= 0xFF; ++page) {
        const uint16_t source = static_cast<uint16_t>((page - 0x20) << 8);
        for (size_t i = 0; i < DmgPpu::OAM_SIZE; ++i)
            workRam.write(static_cast<uint16_t>(source + i),
                          static_cast<uint8_t>(page ^ i));
        bus.write(0xFF46, static_cast<uint8_t>(page));
        dma.advanceTCycles(648);
        expect(bus.read(0xFF46) == page,
               "DMA register retains the written page, not its mapped source");
        for (size_t i = 0; i < DmgPpu::OAM_SIZE; ++i)
            expect(ppu.read(static_cast<uint16_t>(0xFE00 + i)) ==
                       static_cast<uint8_t>(page ^ i),
                   "DMG DMA sources E000-FFFF map to C000-DFFF");
    }
    expect(bus.read(0xFF00) == 0x22,
           "DMA source translation does not change CPU I/O mapping");
}

void testDmaProgressAndBusOwnership() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    AddressBus bus;
    RamRegion ram(0xC000, 0x2000);
    RamRegion hram(0xFF80, 0x7F);
    DmgOamDma dma(bus, ppu);
    bus.map(0xC000, 0xDFFF, ram);
    bus.map(0x8000, 0x9FFF, ppu);
    bus.map(0xFE00, 0xFE9F, ppu);
    bus.map(0xFF46, 0xFF46, dma);
    bus.map(0xFF80, 0xFFFE, hram);
    bus.setCpuBusArbiter(dma);
    unsigned cpuAccesses = 0;
    bus.setCpuBusAccessCallback([&] { ++cpuAccesses; });
    ram.write(0xC000, 0x42);
    ram.write(0xC001, 0x73);
    ram.write(0xC09F, 0xA5);
    bus.write(0xFF46, 0xC0);
    expect(ppu.read(0xFE00) == 0, "DMA does not copy on the FF46 write");
    dma.advanceTCycles(4);
    expect(bus.read(0xFE00) == 0, "OAM remains accessible for the first M-cycle");
    const auto accessesBeforeDma = cpuAccesses;
    dma.advanceTCycles(3);
    expect(ppu.read(0xFE00) == 0, "DMA waits through the partial startup M-cycle");
    dma.advanceTCycles(1);
    expect(cpuAccesses == accessesBeforeDma, "DMA reads never invoke CPU timing callbacks");
    expect(ppu.read(0xFE00) == 0x42 && ppu.read(0xFE01) == 0,
           "the first byte transfers at M=2, without copying the next byte");
    expect(bus.read(0xFE00) == 0xFF, "active DMA blocks CPU OAM reads");
    expect(bus.read(0xC001) == 0x42, "conflicting CPU reads observe the DMA data byte");
    bus.write(0xC001, 0x99);
    expect(ram.read(0xC001) == 0x73, "CPU writes cannot reach the occupied source bus");
    bus.write(0xFE00, 0x99);
    expect(ppu.read(0xFE00) == 0x42, "CPU cannot overwrite OAM during DMA");
    bus.write(0xFF80, 0x31);
    expect(bus.read(0xFF80) == 0x31, "HRAM remains readable and writable during DMA");
    expect(bus.read(0xFF46) == 0xC0, "DMA register readback remains accessible while active");
    bus.write(0x8000, 0x55);
    expect(bus.read(0x8000) == 0x55, "WRAM DMA leaves the separate VRAM bus available");
    ram.write(0xC001, 0x74); // A device-side change before this byte is fetched.
    dma.advanceTCycles(4);
    expect(ppu.read(0xFE01) == 0x74, "DMA reads each byte when its transfer cycle arrives");
    dma.advanceTCycles(4 * 158);
    expect(ppu.read(0xFE9F) == 0xA5 && bus.read(0xFE9F) == 0xFF,
           "the final byte is copied before the last ownership cycle ends");
    dma.advanceTCycles(3);
    expect(bus.read(0xFE00) == 0xFF, "DMA ownership lasts through the final partial M-cycle");
    dma.advanceTCycles(1);
    expect(bus.read(0xFE00) == 0x42 && bus.read(0xC001) == 0x74,
           "OAM and the source bus become CPU-accessible after DMA completes");
    bus.write(0xFF46, 0x80);
    dma.advanceTCycles(8);
    expect(bus.read(0x8001) == 0x55,
           "VRAM DMA drives the conflicted VRAM read with its current byte");
    bus.write(0x8001, 0x99);
    expect(ppu.read(0x8001) == 0, "CPU cannot write the occupied VRAM bus");
    bus.write(0xC001, 0x88);
    expect(bus.read(0xC001) == 0x88,
           "VRAM DMA leaves the separate cartridge/WRAM bus accessible");
}

void testCgbOamDmaCompletesIn160CpuCyclesAtBothSpeeds() {
    for (bool doubleSpeed : {false, true}) {
        InterruptRegisters interrupts;
        CgbVideoRam video;
        DmgPpu ppu(interrupts, &video, true);
        AddressBus bus;
        RamRegion workRam(0xC000, 0x2000);
        DmgOamDma dma(bus, ppu, true);
        IdleCpu cpu;
        SystemClock clock(cpu);
        bus.map(0xC000, 0xDFFF, workRam);
        bus.map(0xFE00, 0xFE9F, ppu);
        bus.map(0xFF46, 0xFF46, dma);
        bus.setCpuBusArbiter(dma);
        for (size_t i = 0; i < DmgPpu::OAM_SIZE; ++i)
            workRam.write(static_cast<uint16_t>(0xC000 + i),
                          static_cast<uint8_t>(0x31 + i * 3));

        clock.addClockedComponent(dma, true);
        clock.setDoubleSpeed(doubleSpeed);
        dma.write(0xFF46, 0xC0);
        const auto first = clock.stepIdle();
        expect(first.tCyclesElapsed == (doubleSpeed ? 2u : 4u) && !dma.isActive(),
               "CGB OAM DMA retains its one-M-cycle startup delay at either speed");
        const auto second = clock.stepIdle();
        expect(second.tCyclesElapsed == (doubleSpeed ? 2u : 4u) && dma.isActive() &&
                   ppu.read(0xFE00) == 0x31 && ppu.read(0xFE01) == 0,
               "CGB OAM DMA copies its first byte at the M=2 boundary");
        const uint64_t firstByteTime = clock.getElapsedTCycles();

        for (size_t i = 1; i < DmgPpu::OAM_SIZE; ++i)
            clock.stepIdle();
        expect(dma.isActive() && ppu.read(0xFE9F) == static_cast<uint8_t>(0x31 + 159 * 3),
               "CGB OAM DMA copies its last byte on the 160th transfer M-cycle");
        clock.stepIdle();
        expect(!dma.isActive() &&
                   clock.getElapsedTCycles() - firstByteTime == (doubleSpeed ? 320u : 640u),
               "CGB OAM DMA releases after 160 CPU M-cycles: 640 normal-speed or 320 double-speed dots");
    }
}

void testDmaRestartKeepsOldTransferUntilNewStart() {
    InterruptRegisters interrupts;
    DmgPpu ppu(interrupts);
    AddressBus bus;
    RamRegion ram(0xC000, 0x2000);
    DmgOamDma dma(bus, ppu);
    bus.map(0xC000, 0xDFFF, ram);
    bus.map(0xFE00, 0xFE9F, ppu);
    bus.map(0xFF46, 0xFF46, dma);
    bus.setCpuBusArbiter(dma);
    ram.write(0xC000, 0x11);
    ram.write(0xC001, 0x22);
    ram.write(0xD000, 0x77);
    ram.write(0xD001, 0x88);
    bus.write(0xFF46, 0xC0);
    dma.advanceTCycles(8);
    bus.write(0xFF46, 0xD0);
    expect(bus.read(0xFF46) == 0xD0 && bus.read(0xFE00) == 0xFF,
           "restart updates FF46 immediately without releasing DMA ownership");
    dma.advanceTCycles(4);
    expect(ppu.read(0xFE00) == 0x11 && ppu.read(0xFE01) == 0x22,
           "old DMA continues one M-cycle after a restart write");
    dma.advanceTCycles(4);
    expect(ppu.read(0xFE00) == 0x77 && ppu.read(0xFE01) == 0x22,
           "new DMA starts from byte zero on the second M-cycle");
    dma.advanceTCycles(4);
    expect(ppu.read(0xFE01) == 0x88, "new DMA proceeds from the replacement page");
}

void testCgbSeparatesCartridgeAndWorkRamBuses() {
    for (uint8_t sourcePage : {uint8_t(0x40), uint8_t(0xA0), uint8_t(0xC0), uint8_t(0xD0)}) {
        InterruptRegisters interrupts;
        DmgPpu ppu(interrupts);
        AddressBus bus;
        RamRegion rom(0, 0x8000), sram(0xA000, 0x2000), wram(0xC000, 0x2000);
        DmgOamDma dma(bus, ppu, true);
        bus.map(0, 0x7FFF, rom);
        bus.map(0xA000, 0xBFFF, sram);
        bus.map(0xC000, 0xDFFF, wram);
        bus.mapAlias(0xE000, 0xFDFF, wram, 0xC000);
        bus.map(0xFE00, 0xFE9F, ppu);
        bus.setCpuBusArbiter(dma);
        rom.write(0x4000, 0x42); sram.write(0xA000, 0xA5);
        wram.write(0xC000, 0xC3); wram.write(0xD000, 0xD4);
        dma.write(0xFF46, sourcePage);
        dma.advanceTCycles(8);
        const bool fromWram = sourcePage >= 0xC0;
        expect(dma.ownsCpuAddress(0x4000) == !fromWram &&
                   dma.ownsCpuAddress(0xA000) == !fromWram &&
                   dma.ownsCpuAddress(0xC000) == fromWram &&
                   dma.ownsCpuAddress(0xD000) == fromWram &&
                   dma.ownsCpuAddress(0xE000) == fromWram &&
                   dma.ownsCpuAddress(0xFDFF) == fromWram,
               "CGB OAM DMA owns only its source bus, including WRAM echo");
        if (fromWram) {
            expect(bus.read(0x4000) == 0x42 && bus.read(0xA000) == 0xA5,
                   "WRAM DMA leaves ROM and cartridge SRAM readable");
            bus.write(0xA000, 0x77); bus.write(0xC000, 0x99);
            expect(sram.read(0xA000) == 0x77 && wram.read(0xC000) == 0xC3,
                   "WRAM DMA permits cartridge writes while blocking its own bus");
        } else {
            expect(bus.read(0xC000) == 0xC3 && bus.read(0xE000) == 0xC3,
                   "cartridge DMA leaves WRAM and its echo readable");
            bus.write(0xE000, 0x66); bus.write(0xA000, 0x99);
            expect(wram.read(0xC000) == 0x66 && sram.read(0xA000) == 0xA5,
                   "cartridge DMA permits WRAM writes while blocking cartridge writes");
        }
        expect(bus.read(0xFE00) == 0xFF && !dma.ownsCpuAddress(0xFF80),
               "CGB DMA still blocks OAM and leaves HRAM accessible");
        dma.advanceTCycles(640);
        expect(!dma.ownsCpuAddress(0x4000) && !dma.ownsCpuAddress(0xC000),
               "completed CGB DMA releases both buses");
    }
}

}

int main() {
    testCgbSeparatesCartridgeAndWorkRamBuses();
    testCgbOamDmaCompletesIn160CpuCyclesAtBothSpeeds();
    testDmaProgressAndBusOwnership();
    testDmaRestartKeepsOldTransferUntilNewStart();
    testDmgOamDmaCopies160BytesFromSelectedPage();
    testDmgOamDmaHighSourcePagesAliasWorkRam();
    std::cout << "DMG OAM DMA tests passed\n";
}
