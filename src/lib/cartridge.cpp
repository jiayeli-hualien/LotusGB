#include "include/cartridge.hpp"

#include <stdexcept>
#include <utility>

namespace LOTUSGB {

RomOnlyCartridge::RomOnlyCartridge(std::vector<uint8_t> rom)
    : rom(std::move(rom)) {
    constexpr size_t maxRomOnlySize = 0x8000;
    if (this->rom.empty() || this->rom.size() > maxRomOnlySize)
        throw std::invalid_argument("ROM-only cartridge must contain 1 to 32 KiB");
}

uint8_t RomOnlyCartridge::read(uint16_t address) {
    if (address >= 0x8000 || address >= rom.size())
        return 0xFF;
    return rom[address];
}

void RomOnlyCartridge::write(uint16_t, uint8_t) {
    // ROM-only cartridges do not expose writable storage or bank registers.
}

}
