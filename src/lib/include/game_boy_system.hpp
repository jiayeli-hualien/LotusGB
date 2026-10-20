#ifndef LOTUSGB_GAME_BOY_SYSTEM_HPP
#define LOTUSGB_GAME_BOY_SYSTEM_HPP

#include <cstdint>
#include <vector>

#include "GBCPU.hpp"
#include "address_bus.hpp"
#include "cartridge.hpp"
#include "decoder.hpp"
#include "dmg_ppu.hpp"
#include "interrupt_registers.hpp"
#include "joypad.hpp"
#include "memory_region.hpp"
#include "system_clock.hpp"
#include "timer.hpp"

namespace LOTUSGB {

enum class SystemStartupMode {
    TestReset,
    DmgPostBoot,
};

struct RetStepFrame {
    bool success = false;
    bool frameCompleted = false;
    uint64_t tCyclesElapsed = 0;
};

// Initial headless DMG composition: ROM-only cartridge, basic RAM regions,
// CPU, address bus, timer, PPU timing/rendering, joypad input, and shared clock.
class GameBoySystem {
public:
    static constexpr uint64_t DMG_FRAME_T_CYCLES = 456 * 154;

    explicit GameBoySystem(
        std::vector<uint8_t> rom,
        SystemStartupMode startupMode = SystemStartupMode::DmgPostBoot);

    RetStepOneCycle stepOneCycle();
    RetStepFrame stepFrame();
    uint64_t getElapsedTCycles() const;
    Reg getCpuRegisters();
    const DmgPpu::FrameBuffer &getFrameBuffer() const;
    void setButtonState(JoypadButton button, bool pressed);

private:
    void initializeDmgPostBootState();

    RomOnlyCartridge cartridge;
    RamRegion workRam;
    RamRegion highRam;
    InterruptRegisters interruptRegisters;
    Joypad joypad;
    Timer timer;
    DmgPpu ppu;
    AddressBus bus;
    Decoder decoder{false};
    Decoder decoderCB{true};
    GBCPU cpu;
    SystemClock systemClock;
};

}

#endif // LOTUSGB_GAME_BOY_SYSTEM_HPP
