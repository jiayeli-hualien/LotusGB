#include <cstdlib>
#include <iostream>

#include "dmg_oam_dma.hpp"
#include "interrupt_registers.hpp"
#include "memory_region.hpp"

namespace {

using namespace LOTUSGB;

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

}

int main() {
    testDmaProgressAndBusOwnership();
    testDmaRestartKeepsOldTransferUntilNewStart();
    testDmgOamDmaCopies160BytesFromSelectedPage();
    testDmgOamDmaHighSourcePagesAliasWorkRam();
    std::cout << "DMG OAM DMA tests passed\n";
}
