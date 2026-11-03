#ifndef LOTUSGB_CGB_COMPAT_ROM_HPP
#define LOTUSGB_CGB_COMPAT_ROM_HPP
#include <cstdint>
#include <initializer_list>
#include <vector>
namespace LOTUSGB::Test {
// Original MIT fixtures: a monochrome cartridge and a color bootstrap that
// selects compatibility mode, programs palettes, then locks them with FF50.
inline std::vector<uint8_t> makeCgbCompatibilityRom() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x147] = 3; rom[0x149] = 2;
    size_t at = 0x150;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) { for (auto b : bytes) rom[at++] = b; };
    const auto io = [&](uint8_t port, uint8_t value) { emit({0x3E, value, 0xE0, port}); };
    emit({0x47, 0x3E, 0x0A, 0xEA, 0, 0, 0x78, 0xEA, 0, 0xA0});
    // Try to reopen native features after bootstrap locking.
    io(0x4C, 0); io(0x6C, 0); io(0x4D, 1); io(0x70, 7); io(0x4F, 1);
    io(0x68, 0x82); io(0x69, 0); io(0x69, 0x7C);
    io(0x74, 0x12); io(0x72, 0x34); io(0x73, 0x56); io(0x75, 0x70);
    size_t offset = 1;
    for (uint8_t port : {0x4C, 0x4D, 0x4F, 0x51, 0x55, 0x56, 0x68, 0x69,
                         0x6A, 0x6B, 0x6C, 0x70, 0x74, 0x76, 0x77, 0x72, 0x73, 0x75}) {
        emit({0xF0, port, 0xEA, static_cast<uint8_t>(offset++), 0xA0});
    }
    emit({0xFA, 0, 0xD0, 0xEA, 0x20, 0xA0, 0xFA, 0, 0x80, 0xEA, 0x21, 0xA0});
    io(2, 0x83); emit({0xF0, 2, 0xEA, 0x22, 0xA0});
    // BGP remaps tile color 1 to compatibility palette entry 2 (green).
    io(0x47, 0xE8); io(0x40, 0x91);
    emit({0x3E, 1, 0xEA, 0x40, 0xA0, 0x18, 0xFE});
    return rom;
}
inline std::vector<uint8_t> makeCgbCompatibilityBootRom() {
    std::vector<uint8_t> boot(0x900, 0);
    boot[0] = 0xC3; boot[1] = 0; boot[2] = 2;
    size_t at = 0x200;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) { for (auto b : bytes) boot[at++] = b; };
    const auto io = [&](uint8_t port, uint8_t value) { emit({0x3E, value, 0xE0, port}); };
    emit({0xF3, 0x31, 0xFE, 0xFF}); io(0x40, 0);
    io(0x4F, 1); emit({0x3E, 0xAA, 0xEA, 0, 0x80});
    io(0x4F, 0);
    for (unsigned row = 0; row < 8; ++row) {
        emit({0x3E, 0xFF, 0xEA, static_cast<uint8_t>(row * 2), 0x80});
    }
    io(0x70, 2); emit({0x3E, 0xAA, 0xEA, 0, 0xD0});
    io(0x70, 1); emit({0x3E, 0x55, 0xEA, 0, 0xD0});
    io(0x70, 2); io(0x4F, 1);
    io(0x4C, 4); io(0x6C, 1);
    // Native palette ports must remain usable between KEY0 and FF50.
    io(0x68, 0x80);
    for (uint8_t byte : {0xFF, 0x7F, 0x1F, 0, 0xE0, 3, 0, 0}) io(0x69, byte);
    emit({0xC3, 0xFC, 0});
    boot[0xFC] = 0x3E; boot[0xFD] = 0x11; boot[0xFE] = 0xE0; boot[0xFF] = 0x50;
    return boot;
}
}
#endif
