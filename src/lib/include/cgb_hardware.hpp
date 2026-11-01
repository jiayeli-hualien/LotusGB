#ifndef LOTUSGB_CGB_HARDWARE_HPP
#define LOTUSGB_CGB_HARDWARE_HPP
#include <array>
#include "address_bus.hpp"
#include "cgb_memory.hpp"
#include "dmg_ppu.hpp"
#include "system_clock.hpp"
namespace LOTUSGB {
class CgbIo final : public IAddressRegion {
    friend class StateCodec;
public:
    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    bool switchArmed = false;
    bool doubleSpeed = false;
    unsigned speedPauseMCycles = 0;
private:
    uint8_t infrared = 0;
    std::array<uint8_t,4> scratch{};
};
class CgbVramDma final : public IAddressRegion, public IClockedComponent {
    friend class StateCodec;
public:
    CgbVramDma(AddressBus &bus, CgbVideoRam &video, DmgPpu &ppu)
        : bus(bus), video(video), ppu(ppu) {}
    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void advanceTCycles(uint64_t tCycles) override;
    void onHBlank();
    bool blocksCpu() const { return active && (!hblank || blockBytes != 0); }
private:
    AddressBus &bus;
    CgbVideoRam &video;
    DmgPpu &ppu;
    uint16_t source = 0, destination = 0x8000;
    unsigned remainingBlocks = 0, blockBytes = 0, byteTCycles = 0;
    bool active = false, hblank = false;
};
}
#endif
