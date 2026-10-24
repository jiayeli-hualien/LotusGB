#include "include/dmg_oam_dma.hpp"

namespace LOTUSGB {

namespace {
constexpr uint16_t DMA_REGISTER_ADDRESS = 0xFF46;
constexpr uint16_t DMA_SOURCE_PAGE_SHIFT = 8;
}

DmgOamDma::DmgOamDma(AddressBus &bus, DmgPpu &ppu) : bus(bus), ppu(ppu) {}

uint8_t DmgOamDma::read(uint16_t address) {
    return address == DMA_REGISTER_ADDRESS ? sourcePage : 0xFF;
}

void DmgOamDma::write(uint16_t address, uint8_t value) {
    if (address != DMA_REGISTER_ADDRESS)
        return;

    sourcePage = value;
    // DMG DMA decodes E0-FF as C0-DF, including FE/FF which the CPU
    // normally routes to OAM/I/O. Keep FF46's readback unmodified.
    const uint8_t mappedPage = value >= 0xE0 ? value - 0x20 : value;
    const uint16_t sourceStart =
        static_cast<uint16_t>(mappedPage) << DMA_SOURCE_PAGE_SHIFT;
    for (size_t offset = 0; offset < DmgPpu::OAM_SIZE; ++offset) {
        const uint8_t byte = bus.read(static_cast<uint16_t>(sourceStart + offset));
        ppu.writeOamFromDma(offset, byte);
    }
}

}
