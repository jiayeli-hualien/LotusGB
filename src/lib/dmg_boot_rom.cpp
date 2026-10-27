#include "include/dmg_boot_rom.hpp"

#include <stdexcept>
#include <utility>

namespace LOTUSGB {

namespace {
constexpr uint16_t BOOT_ROM_END = 0x00FF;
constexpr uint16_t BOOT_ROM_DISABLE_ADDRESS = 0xFF50;
constexpr size_t DMG_BOOT_ROM_SIZE = 0x100;
}

DmgBootRom::DmgBootRom(Cartridge &cartridge, std::vector<uint8_t> bootRom)
    : cartridge(cartridge), bootRom(std::move(bootRom)),
      mapped(!this->bootRom.empty()) {
    if (mapped && this->bootRom.size() != DMG_BOOT_ROM_SIZE)
        throw std::invalid_argument("DMG boot ROM must be exactly 256 bytes");
}

uint8_t DmgBootRom::read(uint16_t address) {
    if (address == BOOT_ROM_DISABLE_ADDRESS)
        return 0xFF;
    if (mapped && address <= BOOT_ROM_END)
        return bootRom[address];
    return cartridge.read(address);
}

void DmgBootRom::write(uint16_t address, uint8_t value) {
    if (address == BOOT_ROM_DISABLE_ADDRESS) {
        if (value != 0)
            mapped = false;
        return;
    }
    if (address <= 0x7FFF)
        cartridge.write(address, value);
}

bool DmgBootRom::isMapped() const {
    return mapped;
}

}
