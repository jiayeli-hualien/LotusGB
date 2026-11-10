#ifndef LOTUSGB_MBC1_MULTICART_ROM_HPP
#define LOTUSGB_MBC1_MULTICART_ROM_HPP
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>
#include "../../src/lib/include/cartridge.hpp"
#include "../../src/lib/include/cartridge_signature.hpp"
namespace LOTUSGB::Test {

struct CartridgeTestAccess {
    static void setMbc1Multicart(Cartridge &cartridge, bool value) {
        cartridge.mbc1Multicart = value;
    }
};

inline std::array<uint8_t, 48> syntheticMbc1MHeader() {
    std::array<uint8_t, 48> header{};
    for (size_t index = 0; index < header.size(); ++index)
        header[index] = static_cast<uint8_t>(0x5A ^ (index * 37));
    return header;
}

inline CartridgeDetail::HeaderFingerprint syntheticMbc1MHeaderFingerprint() {
    const auto header = syntheticMbc1MHeader();
    return StateInternal::sha256(header.data(), header.size());
}

inline bool isSyntheticMbc1Multicart(const std::vector<uint8_t> &rom) {
    return CartridgeDetail::hasRepeatedMbc1MHeaderFingerprint(
        rom, syntheticMbc1MHeaderFingerprint());
}

inline std::vector<uint8_t> makeMbc1MulticartRom() {
    std::vector<uint8_t> rom(0x100000, 0);
    for (unsigned bank = 0; bank < 64; ++bank) rom[bank * 0x4000] = bank;
    const auto header = syntheticMbc1MHeader();
    for (unsigned game = 0; game < 4; ++game) {
        const size_t base = game * 0x40000;
        std::copy(header.begin(), header.end(), rom.begin() + base + 0x104);
        rom[base + 0x100] = 0xC3; rom[base + 0x101] = 0x50; rom[base + 0x102] = 1;
        rom[base + 0x143] = 0x80; rom[base + 0x147] = 3;
        rom[base + 0x148] = 5; rom[base + 0x149] = 2;
    }
    return rom;
}
}
#endif
