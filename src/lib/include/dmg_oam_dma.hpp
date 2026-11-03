#ifndef LOTUSGB_DMG_OAM_DMA_HPP
#define LOTUSGB_DMG_OAM_DMA_HPP

#include <cstdint>

#include "address_bus.hpp"
#include "dmg_ppu.hpp"

namespace LOTUSGB {

// DMG/CGB OAM DMA advances one byte per M-cycle, after its startup delay.
class DmgOamDma final : public IAddressRegion, public IClockedComponent,
                        public ICpuBusArbiter {
    friend class StateCodec;
public:
    DmgOamDma(AddressBus &bus, DmgPpu &ppu, bool cgbMode = false);

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void advanceTCycles(uint64_t tCycles) override;
    bool isActive() const { return active; }
    size_t getPpuReadAddress(size_t address) const;
    bool ownsCpuAddress(uint16_t address) const override;
    uint8_t readConflictedCpuAddress(uint16_t address) const override;

private:
    const bool cgbMode; // Immutable model configuration; snapshot model identifies it.
    AddressBus &bus;
    DmgPpu &ppu;
    uint8_t sourcePage = 0xFF;
    uint8_t activePage = 0;
    uint8_t lastByte = 0xFF;
    unsigned byteOffset = 0;
    unsigned startDelayMCycles = 0;
    unsigned mCycleTCycles = 0;
    bool active = false;
};

}

#endif // LOTUSGB_DMG_OAM_DMA_HPP
