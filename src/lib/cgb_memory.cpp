#include "include/cgb_memory.hpp"

#include <stdexcept>

namespace LOTUSGB {

uint8_t CgbWorkRam::read(uint16_t address) {
    if (address == 0xFF70)
        return static_cast<uint8_t>(0xF8 | bankSelect);
    if (address >= 0xC000 && address <= 0xCFFF)
        return banks[0][address - 0xC000];
    if (address >= 0xD000 && address <= 0xDFFF)
        return banks[bankSelect == 0 ? 1 : bankSelect][address - 0xD000];
    return 0xFF;
}

void CgbWorkRam::write(uint16_t address, uint8_t value) {
    if (address == 0xFF70)
        bankSelect = value & 7;
    else if (address >= 0xC000 && address <= 0xCFFF)
        banks[0][address - 0xC000] = value;
    else if (address >= 0xD000 && address <= 0xDFFF)
        banks[bankSelect == 0 ? 1 : bankSelect][address - 0xD000] = value;
}

uint8_t CgbVideoRam::read(uint16_t address) {
    if (address == 0xFF4F)
        return static_cast<uint8_t>(0xFE | bankSelect);
    return readBank(bankSelect, address);
}

void CgbVideoRam::write(uint16_t address, uint8_t value) {
    if (address == 0xFF4F)
        bankSelect = value & 1;
    else if (address >= 0x8000 && address <= 0x9FFF)
        banks[bankSelect][address - 0x8000] = value;
}

uint8_t CgbVideoRam::readBank(uint8_t bank, uint16_t address) const {
    if (bank >= banks.size())
        throw std::out_of_range("CGB VRAM bank must be 0 or 1");
    if (address < 0x8000 || address > 0x9FFF)
        return 0xFF;
    return banks[bank][address - 0x8000];
}

}
