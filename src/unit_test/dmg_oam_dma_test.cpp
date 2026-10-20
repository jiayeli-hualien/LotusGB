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

}

int main() {
    testDmgOamDmaCopies160BytesFromSelectedPage();
    std::cout << "DMG OAM DMA tests passed\n";
}
