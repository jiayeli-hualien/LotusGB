#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace TestRom {

// Original synthetic ROM: repeatedly disable LCD in VBlank, wait ~24K
// T-cycles, re-enable it and increment battery SRAM. Every enabled frame is
// uniformly black; LCD-off output is white. No external ROM/assets required.
inline std::vector<uint8_t> lcdRestartCadence() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 0x01;
    rom[0x147] = 0x03; // MBC1 + RAM + battery
    rom[0x149] = 0x02; // 8 KiB SRAM
    const std::vector<uint8_t> program = {
        0xF3,                         // DI
        0x3E, 0x0A, 0xEA, 0x00, 0x00, // enable cartridge RAM
        0x21, 0x00, 0xA0, 0x36, 0x00, // HL=$A000, counter=0
        0x3E, 0xFF, 0xE0, 0x47,       // BGP: all black
        0xF0, 0x44, 0xFE, 0x90, 0x38, 0xFA, // loop: wait for VBlank
        0xAF, 0xE0, 0x40,             // LCD off
        0x01, 0x00, 0x04,             // BC=1024
        0x0B, 0x78, 0xB1, 0x20, 0xFB, // bounded delay
        0x3E, 0x91, 0xE0, 0x40,       // LCD + BG on
        0x34,                         // increment SRAM counter
        0x18, 0xE8,                   // JR loop
    };
    std::copy(program.begin(), program.end(), rom.begin() + 0x150);
    return rom;
}

}
