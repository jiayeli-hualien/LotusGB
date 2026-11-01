#ifndef LOTUSGB_CGB_MEMORY_HPP
#define LOTUSGB_CGB_MEMORY_HPP

#include <array>
#include <cstdint>

#include "address_bus.hpp"
#include "video_ram.hpp"

namespace LOTUSGB {

// Unlocked CGB-mode memory components. Constructors provide deterministic
// zero-filled test state, not a hardware power-on/boot-ROM profile. System
// model gating is owned by composition; CPU VRAM windows are owned by the PPU.
class CgbWorkRam final : public IAddressRegion {
    friend class StateCodec;
public:
    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;

private:
    std::array<std::array<uint8_t, 0x1000>, 8> banks{};
    uint8_t bankSelect = 0; // Raw SVBK=0, effective bank=1.
};

class CgbVideoRam final : public IVideoRam {
    friend class StateCodec;
public:
    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    // PPU fetches select a bank independently of the CPU's VBK. This
    // view does not change VBK or apply CPU mode-3 access restrictions.
    uint8_t readBank(uint8_t bank, uint16_t address) const override;

private:
    std::array<std::array<uint8_t, 0x2000>, 2> banks{};
    uint8_t bankSelect = 0;
};

}

#endif
