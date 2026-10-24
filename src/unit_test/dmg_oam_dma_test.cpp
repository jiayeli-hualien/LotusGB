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

}

int main() {
    testDmgOamDmaCopies160BytesFromSelectedPage();
    testDmgOamDmaHighSourcePagesAliasWorkRam();
    std::cout << "DMG OAM DMA tests passed\n";
}
