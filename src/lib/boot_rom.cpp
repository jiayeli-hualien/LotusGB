#include "include/boot_rom.hpp"

#include <stdexcept>
#include <utility>

namespace LOTUSGB {

namespace {
constexpr uint16_t BOOT_ROM_END = 0x00FF;
constexpr uint16_t BOOT_ROM_DISABLE_ADDRESS = 0xFF50;
constexpr size_t DMG_BOOT_ROM_SIZE = 0x100;
}

BootRom::BootRom(Cartridge &cartridge, std::vector<uint8_t> bootRom, HardwareModel model)
    : cartridge(cartridge), bootRom(std::move(bootRom)),
      mapped(!this->bootRom.empty()), model(model) {
    const size_t expectedSize = model == HardwareModel::Dmg ? DMG_BOOT_ROM_SIZE : 0x900;
    if (mapped && this->bootRom.size() != expectedSize)
        throw std::invalid_argument(model == HardwareModel::Dmg
            ? "DMG boot ROM must be exactly 256 bytes"
            : "CGB boot ROM must use the address-indexed 2304-byte layout");
}

uint8_t BootRom::read(uint16_t address) {
    if (address == BOOT_ROM_DISABLE_ADDRESS)
        return 0xFF;
    if (mapped && (address <= BOOT_ROM_END
        || (model == HardwareModel::CgbE && address >= 0x0200 && address <= 0x08FF)))
        return bootRom[address];
    return cartridge.read(address);
}

void BootRom::write(uint16_t address, uint8_t value) {
    if (address == BOOT_ROM_DISABLE_ADDRESS) {
        if (value != 0)
            mapped = false;
        return;
    }
    if (address <= 0x7FFF)
        cartridge.write(address, value);
}

bool BootRom::isMapped() const {
    return mapped;
}

}
