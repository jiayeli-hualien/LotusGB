#include "include/cgb_hardware.hpp"
#include <algorithm>
namespace LOTUSGB {
uint8_t CgbIo::read(uint16_t address) {
    if (address == 0xFF4C) return 0xFF; // KEY0 is boot-only, write-only.
    if (address == 0xFF6C) return nativeFeaturesEnabled() ? static_cast<uint8_t>(0xFE | opri) : 0xFF;
    if (!nativeFeaturesEnabled() && (address == 0xFF4D || address == 0xFF56 || address == 0xFF74)) return 0xFF;
    if (address == 0xFF4D) return static_cast<uint8_t>(0x7E | (doubleSpeed ? 0x80 : 0) | switchArmed);
    if (address == 0xFF56) {
        const bool receiveEnabled = (infrared & 0xC0) == 0xC0;
        const bool receivedLight = receiveEnabled && infraredSettleTCycles == 0 && infraredInputLatched;
        return static_cast<uint8_t>(0x3C | (infrared & 0xC1) | (receivedLight ? 0 : 0x02));
    }
    if (address >= 0xFF72 && address <= 0xFF75)
        return address == 0xFF75 ? static_cast<uint8_t>(0x8F | scratch[3]) : scratch[address - 0xFF72];
    return 0xFF;
}
void CgbIo::write(uint16_t address, uint8_t value) {
    if (address == 0xFF4C || address == 0xFF6C) {
        if (boot && boot->isMapped()) (address == 0xFF4C ? key0 : opri) = value & (address == 0xFF4C ? 4 : 1);
        return;
    }
    if (!nativeFeaturesEnabled() && (address == 0xFF4D || address == 0xFF56 || address == 0xFF74)) return;
    if (address == 0xFF4D) switchArmed = (value & 1) != 0;
    else if (address == 0xFF56) {
        const bool wasEnabled = (infrared & 0xC0) == 0xC0;
        infrared = value & 0xC1;
        const bool receiveEnabled = (infrared & 0xC0) == 0xC0;
        if (!receiveEnabled) {
            infraredSettleTCycles = 0;
            infraredInputLatched = false;
        } else if (!wasEnabled) {
            infraredSettleTCycles = IR_RECEIVER_SETTLE_T_CYCLES;
            infraredInputLatched = false;
        }
    }
    else if (address >= 0xFF72 && address <= 0xFF75)
        scratch[address - 0xFF72] = address == 0xFF75 ? value & 0x70 : value;
}
void CgbIo::advanceTCycles(uint64_t tCycles) {
    if (!nativeFeaturesEnabled() || (infrared & 0xC0) != 0xC0) {
        infraredInputLatched = false;
        return;
    }
    bool receiverJustSettled = false;
    if (infraredSettleTCycles != 0) {
        if (tCycles < infraredSettleTCycles) {
            infraredSettleTCycles -= static_cast<uint32_t>(tCycles);
            return;
        }
        tCycles -= infraredSettleTCycles;
        infraredSettleTCycles = 0;
        receiverJustSettled = true;
    }
    if (tCycles != 0 || receiverJustSettled)
        infraredInputLatched = infraredInputCallback && infraredInputCallback();
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
        hblank = (value & 0x80) != 0;
        active = true;
        // With the LCD off, HBlank DMA transfers one block, then waits for the
        // next visible HBlank after LCD operation resumes.
        blockBytes = hblank && (ppu.read(0xFF40) & 0x80) ? 0 : 16;
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
void CgbVramDma::onLcdDisabled() {
    if (active && hblank && blockBytes == 0) { blockBytes = 16; byteTCycles = 0; }
}
void CgbVramDma::advanceTCycles(uint64_t cycles) {
    while (cycles-- && active && blockBytes) {
        if (++byteTCycles < 2) continue;
        byteTCycles = 0;
        const uint8_t value = source < 0x8000 || (source >= 0xA000 && source < 0xE000)
            ? bus.readForDma(source) : 0xFF;
        if (!ppu.isVramWriteBlocked()) video.write(destination, value);
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
