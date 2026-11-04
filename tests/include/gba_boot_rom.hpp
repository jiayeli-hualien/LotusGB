#ifndef LOTUSGBA_TEST_GBA_BOOT_ROM_HPP
#define LOTUSGBA_TEST_GBA_BOOT_ROM_HPP

#include <cstdint>
#include <vector>

// Original minimal ARM-mode test cartridge fixture. MIT licensed with LotusGB.
inline std::vector<uint8_t> makeGbaMode3BootRom() {
    std::vector<uint8_t> rom(0x100, 0);
    const auto put32 = [&rom](size_t offset, uint32_t value) {
        rom[offset] = static_cast<uint8_t>(value);
        rom[offset + 1] = static_cast<uint8_t>(value >> 8);
        rom[offset + 2] = static_cast<uint8_t>(value >> 16);
        rom[offset + 3] = static_cast<uint8_t>(value >> 24);
    };

    // Cartridge entry branches over the standard 0xC0-byte header area.
    put32(0x00, 0xEA00002E); // B 0x080000C0
    const uint32_t code[] = {
        0xE59F0020, // LDR r0, [pc, #32] ; DISPCNT
        0xE59F1020, // LDR r1, [pc, #32] ; Mode 3 + BG2
        0xE1C010B0, // STRH r1, [r0]
        0xE59F001C, // LDR r0, [pc, #28] ; VRAM
        0xE59F101C, // LDR r1, [pc, #28] ; red BGR555
        0xE59F201C, // LDR r2, [pc, #28] ; 240 * 160 pixels
        0xE0C010B2, // STRH r1, [r0], #2
        0xE2522001, // SUBS r2, r2, #1
        0x1AFFFFFC, // BNE fill
        0xEAFFFFFE, // B .
        0x04000000,
        0x00000403,
        0x06000000,
        0x0000001F,
        240 * 160,
    };
    for (size_t i = 0; i < sizeof(code) / sizeof(code[0]); ++i)
        put32(0xC0 + i * 4, code[i]);
    return rom;
}

// Exercises an ARM entry branch and BX handoff before running the pixel loop
// with ordinary Thumb instructions, as a second boot-path regression.
inline std::vector<uint8_t> makeGbaThumbMode3BootRom() {
    std::vector<uint8_t> rom(0x134, 0);
    const auto put16 = [&rom](size_t offset, uint16_t value) {
        rom[offset] = static_cast<uint8_t>(value);
        rom[offset + 1] = static_cast<uint8_t>(value >> 8);
    };
    const auto put32 = [&put16](size_t offset, uint32_t value) {
        put16(offset, static_cast<uint16_t>(value));
        put16(offset + 2, static_cast<uint16_t>(value >> 16));
    };

    put32(0x00, 0xEA00002E); // ARM entry: B 0x080000C0
    put32(0xC0, 0xE59F3004); // LDR r3, [pc, #4]
    put32(0xC4, 0xE12FFF13); // BX r3
    put32(0xC8, 0xEAFFFFFE); // ARM fallback loop
    put32(0xCC, 0x08000101); // Thumb entry with state bit set

    const uint16_t thumb[] = {
        0x4807, // LDR r0, [pc, #28] ; DISPCNT at 0x120
        0x4908, // LDR r1, [pc, #32] ; mode value at 0x124
        0x8001, // STRH r1, [r0]
        0x4808, // LDR r0, [pc, #32] ; VRAM at 0x128
        0x4908, // LDR r1, [pc, #32] ; red at 0x12C
        0x4A09, // LDR r2, [pc, #36] ; pixel count at 0x130
        0x8001, // STRH r1, [r0]
        0x3002, // ADDS r0, #2
        0x3A01, // SUBS r2, #1
        0xD1FB, // BNE fill
        0xE7FE, // B .
    };
    for (size_t i = 0; i < sizeof(thumb) / sizeof(thumb[0]); ++i)
        put16(0x100 + i * 2, thumb[i]);
    put32(0x120, 0x04000000);
    put32(0x124, 0x00000403);
    put32(0x128, 0x06000000);
    put32(0x12C, 0x0000001F);
    put32(0x130, 240 * 160);
    return rom;
}

#endif // LOTUSGBA_TEST_GBA_BOOT_ROM_HPP
