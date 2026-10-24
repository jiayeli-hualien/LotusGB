#ifndef LOTUSGB_DMG_OAM_DMA_HPP
#define LOTUSGB_DMG_OAM_DMA_HPP

#include <cstdint>

#include "address_bus.hpp"
#include "dmg_ppu.hpp"

namespace LOTUSGB {

// DMG OAM DMA advances one byte per M-cycle, after its startup delay.
class DmgOamDma final : public IAddressRegion, public IClockedComponent,
                        public ICpuBusArbiter {
public:
    DmgOamDma(AddressBus &bus, DmgPpu &ppu);

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void advanceTCycles(uint64_t tCycles) override;
    bool ownsCpuAddress(uint16_t address) const override;
    uint8_t readConflictedCpuAddress(uint16_t address) const override;

private:
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
