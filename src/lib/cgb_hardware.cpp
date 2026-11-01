#include "include/cgb_hardware.hpp"
namespace LOTUSGB {
uint8_t CgbIo::read(uint16_t address) {
    if (address == 0xFF4D) return static_cast<uint8_t>(0x7E | (doubleSpeed ? 0x80 : 0) | switchArmed);
    if (address == 0xFF56) return static_cast<uint8_t>(0x3E | infrared);
    if (address >= 0xFF72 && address <= 0xFF75)
        return address == 0xFF75 ? static_cast<uint8_t>(0x8F | scratch[3]) : scratch[address - 0xFF72];
    return 0xFF;
}
void CgbIo::write(uint16_t address, uint8_t value) {
    if (address == 0xFF4D) switchArmed = (value & 1) != 0;
    else if (address == 0xFF56) infrared = value & 0xC1;
    else if (address >= 0xFF72 && address <= 0xFF75)
        scratch[address - 0xFF72] = address == 0xFF75 ? value & 0x70 : value;
}
uint8_t CgbVramDma::read(uint16_t address) {
    if (address == 0xFF55)
        return static_cast<uint8_t>((active ? 0 : 0x80) | ((remainingBlocks - 1) & 0x7F));
    return 0xFF; // HDMA1-4 are write-only.
}
void CgbVramDma::write(uint16_t address, uint8_t value) {
    if (address == 0xFF55) {
        if (active) {
            if (hblank && !(value & 0x80)) { active = false; blockBytes = 0; }
            return;
        }
        remainingBlocks = (value & 0x7F) + 1;
        hblank = (value & 0x80) && (ppu.read(0xFF40) & 0x80);
        active = true;
        blockBytes = hblank ? 0 : 16;
        byteTCycles = 0;
        return;
    }
    if (active) return;
    switch (address) {
    case 0xFF51: source = static_cast<uint16_t>((value << 8) | (source & 0xF0)); break;
    case 0xFF52: source = static_cast<uint16_t>((source & 0xFF00) | (value & 0xF0)); break;
    case 0xFF53: destination = static_cast<uint16_t>(0x8000 | ((value & 0x1F) << 8) | (destination & 0xF0)); break;
    case 0xFF54: destination = static_cast<uint16_t>((destination & 0xFF00) | (value & 0xF0)); break;
    default: break;
    }
}
void CgbVramDma::onHBlank() {
    if (active && hblank && blockBytes == 0) { blockBytes = 16; byteTCycles = 0; }
}
void CgbVramDma::advanceTCycles(uint64_t cycles) {
    while (cycles-- && active && blockBytes) {
        if (++byteTCycles < 2) continue;
        byteTCycles = 0;
        const uint8_t value = source < 0x8000 || (source >= 0xA000 && source < 0xE000)
            ? bus.readForDma(source) : 0xFF;
        video.write(destination, value);
        ++source;
        ++destination;
        if (--blockBytes == 0) {
            --remainingBlocks;
            if (remainingBlocks == 0 || destination == 0xA000) {
                active = false;
                remainingBlocks = 0;
                destination = static_cast<uint16_t>(0x8000 | (destination & 0x1FFF));
            } else if (!hblank) blockBytes = 16;
        }
    }
}
}
