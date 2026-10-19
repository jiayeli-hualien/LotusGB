#ifndef LOTUSGB_GAME_BOY_SYSTEM_HPP
#define LOTUSGB_GAME_BOY_SYSTEM_HPP

#include <cstdint>
#include <vector>

#include "GBCPU.hpp"
#include "address_bus.hpp"
#include "cartridge.hpp"
#include "decoder.hpp"
#include "interrupt_registers.hpp"
#include "joypad.hpp"
#include "memory_region.hpp"
#include "system_clock.hpp"
#include "timer.hpp"

namespace LOTUSGB {

// Initial headless DMG composition: ROM-only cartridge, basic RAM regions,
// CPU, address bus, timer, joypad input, and shared clock. PPU is pending.
class GameBoySystem {
public:
    explicit GameBoySystem(std::vector<uint8_t> rom);

    RetStepOneCycle stepOneCycle();
    uint64_t getElapsedTCycles() const;
    Reg getCpuRegisters();
    void setButtonState(JoypadButton button, bool pressed);

private:
    RomOnlyCartridge cartridge;
    RamRegion videoRam;
    RamRegion workRam;
    RamRegion objectRam;
    RamRegion highRam;
    InterruptRegisters interruptRegisters;
    Joypad joypad;
    Timer timer;
    AddressBus bus;
    Decoder decoder{false};
    Decoder decoderCB{true};
    GBCPU cpu;
    SystemClock systemClock;
};

}

#endif // LOTUSGB_GAME_BOY_SYSTEM_HPP
