#ifndef LOTUSGB_DMG_BOOT_ROM_HPP
#define LOTUSGB_DMG_BOOT_ROM_HPP

#include <cstdint>
#include <vector>

#include "address_bus.hpp"
#include "cartridge.hpp"

namespace LOTUSGB {

// DMG boot ROM overlay for $0000-$00FF and the one-way $FF50 disable latch.
class DmgBootRom final : public IAddressRegion {
public:
    DmgBootRom(Cartridge &cartridge, std::vector<uint8_t> bootRom);

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    bool isMapped() const;

private:
    Cartridge &cartridge;
    std::vector<uint8_t> bootRom;
    bool mapped;
};

}

#endif // LOTUSGB_DMG_BOOT_ROM_HPP
