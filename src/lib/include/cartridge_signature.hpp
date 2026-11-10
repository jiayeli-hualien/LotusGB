#ifndef LOTUSGB_CARTRIDGE_SIGNATURE_HPP
#define LOTUSGB_CARTRIDGE_SIGNATURE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "state_hash.hpp"

namespace LOTUSGB::CartridgeDetail {

using HeaderFingerprint = std::array<uint8_t, 32>;

inline bool hasRepeatedMbc1MHeaderFingerprint(
    const std::vector<uint8_t> &rom,
    const HeaderFingerprint &expectedFingerprint) {
    constexpr size_t ROM_SIZE = 0x100000;
    constexpr size_t LOGO_OFFSET = 0x0104;
    constexpr size_t SUBGAME_LOGO_OFFSET = 0x40104;
    constexpr size_t LOGO_SIZE = 48;
    if (rom.size() != ROM_SIZE) return false;

    const auto firstFingerprint =
        StateInternal::sha256(rom.data() + LOGO_OFFSET, LOGO_SIZE);
    const auto subgameFingerprint =
        StateInternal::sha256(rom.data() + SUBGAME_LOGO_OFFSET, LOGO_SIZE);
    return firstFingerprint == expectedFingerprint &&
           subgameFingerprint == expectedFingerprint;
}

} // namespace LOTUSGB::CartridgeDetail

#endif // LOTUSGB_CARTRIDGE_SIGNATURE_HPP
