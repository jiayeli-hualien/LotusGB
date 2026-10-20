#ifndef LOTUSGB_DMG_OAM_DMA_HPP
#define LOTUSGB_DMG_OAM_DMA_HPP

#include <cstdint>

#include "address_bus.hpp"
#include "dmg_ppu.hpp"

namespace LOTUSGB {

// Initial OAM DMA implementation. The 160-byte transfer is immediate; CPU
// blocking and per-byte bus timing are not modeled yet.
class DmgOamDma final : public IAddressRegion {
public:
    DmgOamDma(AddressBus &bus, DmgPpu &ppu);

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;

private:
    AddressBus &bus;
    DmgPpu &ppu;
    uint8_t sourcePage = 0xFF;
};

}

#endif // LOTUSGB_DMG_OAM_DMA_HPP
