#ifndef LOTUSGBA_TEST_GBA_BOOT_ROM_HPP
#define LOTUSGBA_TEST_GBA_BOOT_ROM_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

// Original minimal ARM-mode test cartridge fixture. MIT licensed with LotusGB.
inline std::vector<uint8_t> makeGbaMode3BootRom(uint16_t color = 0x001F) {
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
        0xE59F101C, // LDR r1, [pc, #28] ; BGR555 pixel color
        0xE59F201C, // LDR r2, [pc, #28] ; 240 * 160 pixels
        0xE0C010B2, // STRH r1, [r0], #2
        0xE2522001, // SUBS r2, r2, #1
        0x1AFFFFFC, // BNE fill
        0xEAFFFFFE, // B .
        0x04000000,
        0x00000403,
        0x06000000,
        color,
        240 * 160,
    };
    for (size_t i = 0; i < sizeof(code) / sizeof(code[0]); ++i)
        put32(0xC0 + i * 4, code[i]);
    return rom;
}

// Exercises an ARM entry branch and BX handoff before running the pixel loop
// with ordinary Thumb instructions, as a second boot-path regression.
inline std::vector<uint8_t> makeGbaThumbMode3BootRom(
    uint16_t color = 0x001F) {
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
    put32(0x12C, color);
    put32(0x130, 240 * 160);
    return rom;
}

// Original ARM homebrew scene for a tiled BG and regular OBJ. The same
// fixture is run through GbaSystem and the libretro video callback.
inline std::vector<uint8_t> makeGbaMode0TileSpriteHomebrewRom() {
    constexpr uint32_t DISPCNT = 0x04000000;
    constexpr uint32_t BG0CNT = 0x04000008;
    constexpr uint32_t PALETTE = 0x05000000;
    constexpr uint32_t VRAM = 0x06000000;
    constexpr uint32_t OBJ_PALETTE = 0x05000200;
    constexpr uint32_t OAM = 0x07000000;

    struct CopyBlock {
        uint32_t destination;
        uint32_t count;
        std::vector<uint8_t> data;
    };
    const auto halfwords = [](std::initializer_list<uint16_t> values) {
        std::vector<uint8_t> data;
        data.reserve(values.size() * 2);
        for (uint16_t value : values) {
            data.push_back(static_cast<uint8_t>(value));
            data.push_back(static_cast<uint8_t>(value >> 8));
        }
        return data;
    };
    const std::vector<CopyBlock> blocks = {
        {DISPCNT, 1, halfwords({0x1140})}, // Mode 0, BG0, OBJ, 1D OBJ mapping.
        {BG0CNT, 1, halfwords({0x0004})}, // BG0 character block 1.
        {PALETTE, 2, halfwords({0x0000, 0x001F})}, // Black and red.
        {VRAM + 0x4000, 16, std::vector<uint8_t>(32, 0x11)},
        {OBJ_PALETTE, 2, halfwords({0x0000, 0x03E0})}, // Black and green.
        {VRAM + 0x10000, 64, std::vector<uint8_t>(128, 0x11)},
        {OAM, 4, halfwords({32, static_cast<uint16_t>(40 | (1u << 14)), 0, 0})},
    };

    constexpr size_t codeOffset = 0xC0;
    const size_t codeSize = blocks.size() * 4 * sizeof(uint32_t) +
                            sizeof(uint32_t);
    const size_t literalOffset = codeOffset + codeSize;
    const size_t assetOffset = literalOffset + blocks.size() * 3 * sizeof(uint32_t);
    std::vector<uint8_t> rom(assetOffset, 0);
    const auto write32 = [&rom](size_t offset, uint32_t value) {
        rom[offset] = static_cast<uint8_t>(value);
        rom[offset + 1] = static_cast<uint8_t>(value >> 8);
        rom[offset + 2] = static_cast<uint8_t>(value >> 16);
        rom[offset + 3] = static_cast<uint8_t>(value >> 24);
    };
    write32(0, 0xEA00002E); // Cartridge entry branches to 0x080000C0.

    size_t assetCursor = assetOffset;
    for (size_t index = 0; index < blocks.size(); ++index) {
        const size_t instructionOffset = codeOffset +
            index * 4 * sizeof(uint32_t);
        const size_t blockLiteralOffset = literalOffset +
            index * 3 * sizeof(uint32_t);
        for (unsigned reg = 0; reg < 3; ++reg) {
            const size_t literalAddress = blockLiteralOffset +
                reg * sizeof(uint32_t);
            const size_t pcAddress = instructionOffset +
                reg * sizeof(uint32_t) + 8;
            const uint32_t immediate = static_cast<uint32_t>(
                literalAddress - pcAddress);
            write32(instructionOffset + reg * sizeof(uint32_t),
                    0xE59F0000 | (reg << 12) | immediate);
        }
        write32(instructionOffset + 12, 0xEF00000B); // BIOS CpuSet.
        write32(blockLiteralOffset,
                0x08000000 + static_cast<uint32_t>(assetCursor));
        write32(blockLiteralOffset + 4, blocks[index].destination);
        write32(blockLiteralOffset + 8, blocks[index].count);
        rom.resize(assetCursor + blocks[index].data.size(), 0);
        std::copy(blocks[index].data.begin(), blocks[index].data.end(),
                  rom.begin() + static_cast<std::ptrdiff_t>(assetCursor));
        assetCursor = (assetCursor + blocks[index].data.size() + 3) &
                      ~size_t{3};
        rom.resize(assetCursor, 0);
    }

    write32(codeOffset + blocks.size() * 4 * sizeof(uint32_t), 0xEAFFFFFE);
    return rom;
}

#endif // LOTUSGBA_TEST_GBA_BOOT_ROM_HPP
