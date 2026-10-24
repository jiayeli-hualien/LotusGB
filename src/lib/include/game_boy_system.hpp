#ifndef LOTUSGB_GAME_BOY_SYSTEM_HPP
#define LOTUSGB_GAME_BOY_SYSTEM_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "GBCPU.hpp"
#include "address_bus.hpp"
#include "cartridge.hpp"
#include "decoder.hpp"
#include "dmg_apu.hpp"
#include "dmg_ppu.hpp"
#include "dmg_serial.hpp"
#include "dmg_oam_dma.hpp"
#include "interrupt_registers.hpp"
#include "joypad.hpp"
#include "memory_region.hpp"
#include "system_clock.hpp"
#include "timer.hpp"

namespace LOTUSGB {

struct RetStepFrame {
    bool success = false;
    bool frameCompleted = false;
    uint64_t tCyclesElapsed = 0;
};

// Initial headless DMG composition: cartridge, basic RAM regions,
// CPU, address bus, timer, PPU timing/rendering, joypad input, and shared clock.
class GameBoySystem {
public:
    static constexpr uint64_t DMG_FRAME_T_CYCLES = 456 * 154;

    explicit GameBoySystem(std::vector<uint8_t> rom);
    GameBoySystem(std::vector<uint8_t> rom,
                  std::shared_ptr<std::vector<uint8_t>> saveRam);
    GameBoySystem(std::vector<uint8_t> rom,
                  std::shared_ptr<std::vector<uint8_t>> saveRam,
                  std::shared_ptr<std::vector<uint8_t>> saveRtc);
    // Build a system from the synthetic CPU-reset state for unit tests.
    static GameBoySystem createForTest(std::vector<uint8_t> rom);

    RetStepOneCycle stepOneCycle();
    RetStepFrame stepFrame();
    uint64_t getElapsedTCycles() const;
    Reg getCpuRegisters();
    const DmgPpu::FrameBuffer &getFrameBuffer() const;
    std::vector<int16_t> takeAudioSamples();
    void setButtonState(JoypadButton button, bool pressed);
    uint8_t *getSaveRamData();
    size_t getSaveRamSize() const;
    std::shared_ptr<std::vector<uint8_t>> getSaveRamStorage() const;
    uint8_t *getRtcData();
    size_t getRtcSize() const;
    std::shared_ptr<std::vector<uint8_t>> getRtcStorage() const;

private:
    enum class StartupProfile {
        TestReset,
        DmgPostBoot,
    };

    GameBoySystem(std::vector<uint8_t> rom, StartupProfile startupProfile,
                  std::shared_ptr<std::vector<uint8_t>> saveRam,
                  std::shared_ptr<std::vector<uint8_t>> saveRtc);
    void initializeDmgPostBootState();

    Cartridge cartridge;
    RamRegion workRam;
    RamRegion highRam;
    InterruptRegisters interruptRegisters;
    Joypad joypad;
    DmgSerial serial;
    Timer timer;
    DmgApu apu;
    DmgPpu ppu;
    AddressBus bus;
    DmgOamDma oamDma;
    Decoder decoder{false};
    Decoder decoderCB{true};
    GBCPU cpu;
    SystemClock systemClock;
};

}

#endif // LOTUSGB_GAME_BOY_SYSTEM_HPP
