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
    // A fresh transfer starts on M=2 after the write. A running transfer
    // continues through M=1; the new page takes over at the same M=2 edge.
    startDelayMCycles = 2;
}

void DmgOamDma::advanceTCycles(uint64_t tCycles) {
    for (uint64_t cycle = 0; cycle < tCycles; ++cycle) {
        if (++mCycleTCycles < 4)
            continue;
        mCycleTCycles = 0;
        if (startDelayMCycles > 0 && --startDelayMCycles == 0) {
            activePage = sourcePage >= 0xE0 ? sourcePage - 0x20 : sourcePage;
            byteOffset = 0;
            active = true;
        }
        if (!active)
            continue;
        if (byteOffset == DmgPpu::OAM_SIZE) {
            active = false;
            continue;
        }
        const uint16_t source = static_cast<uint16_t>(
            (static_cast<uint16_t>(activePage) << DMA_SOURCE_PAGE_SHIFT) + byteOffset);
        lastByte = bus.readForDma(source);
        ppu.writeOamFromDma(byteOffset++, lastByte);
    }
}

bool DmgOamDma::ownsCpuAddress(uint16_t address) const {
    if (!active || address >= 0xFF00)
        return false;
    if (address >= 0xFE00)
        return true;
    // DMG shares the cartridge/WRAM bus; VRAM has a separate bus.
    const bool cpuVram = address >= 0x8000 && address < 0xA000;
    const bool dmaVram = activePage >= 0x80 && activePage < 0xA0;
    return cpuVram == dmaVram;
}

uint8_t DmgOamDma::readConflictedCpuAddress(uint16_t address) const {
    return address >= 0xFE00 ? 0xFF : lastByte;
}

}
