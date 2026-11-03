#ifndef LOTUSGB_CGB_HARDWARE_HPP
#define LOTUSGB_CGB_HARDWARE_HPP
#include <array>
#include <functional>
#include <utility>
#include "address_bus.hpp"
#include "boot_rom.hpp"
#include "cgb_memory.hpp"
#include "dmg_ppu.hpp"
#include "system_clock.hpp"
namespace LOTUSGB {
class CgbIo final : public IAddressRegion, public IClockedComponent {
    friend class StateCodec;
public:
    static constexpr uint32_t IR_RECEIVER_SETTLE_T_CYCLES = (4194304u * 50u + 999u) / 1000u;
    explicit CgbIo(BootRom *boot = nullptr) : boot(boot) {}
    bool nativeFeaturesEnabled() const { return (boot && boot->isMapped()) || !(key0 & 4); }
    bool xPriority() const { return opri != 0; }
    void initializeCompatibility() { key0 = 4; opri = 1; }
    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void advanceTCycles(uint64_t tCycles) override;
    bool infraredLedOn() const { return (infrared & 1) != 0; }
    void setInfraredInputCallback(std::function<bool()> callback) { infraredInputCallback = std::move(callback); }
    bool switchArmed = false;
    bool doubleSpeed = false;
    unsigned speedPauseMCycles = 0;
private:
    BootRom *boot;
    uint8_t key0 = 0, opri = 0;
    uint8_t infrared = 0;
    uint32_t infraredSettleTCycles = 0;
    bool infraredInputLatched = false;
    std::function<bool()> infraredInputCallback;
    std::array<uint8_t,4> scratch{};
};
// Borrowed mode/target wiring. Boot code retains native register access until
// FF50 locks the selected mode; compatibility cartridges cannot reopen it.
class CgbNativeRegisterRegion final : public IAddressRegion {
public:
    CgbNativeRegisterRegion(CgbIo &mode, IAddressRegion &target) : mode(mode), target(target) {}
    uint8_t read(uint16_t address) override { return mode.nativeFeaturesEnabled() ? target.read(address) : 0xFF; }
    void write(uint16_t address, uint8_t value) override { if (mode.nativeFeaturesEnabled()) target.write(address, value); }
private:
    CgbIo &mode;
    IAddressRegion &target;
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
    void onLcdDisabled();
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
