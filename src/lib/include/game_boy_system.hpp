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
#include "dmg_boot_rom.hpp"
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
    friend class StateCodec;
public:
    static constexpr uint64_t DMG_FRAME_T_CYCLES = 456 * 154;

    explicit GameBoySystem(std::vector<uint8_t> rom);
    GameBoySystem(std::vector<uint8_t> rom,
                  std::shared_ptr<std::vector<uint8_t>> saveRam);
    GameBoySystem(std::vector<uint8_t> rom,
                  std::shared_ptr<std::vector<uint8_t>> saveRam,
                  std::shared_ptr<std::vector<uint8_t>> saveRtc);
    GameBoySystem(std::vector<uint8_t> rom,
                  std::shared_ptr<std::vector<uint8_t>> saveRam,
                  std::shared_ptr<std::vector<uint8_t>> saveRtc,
                  std::vector<uint8_t> dmgBootRom);
    // Build a system from the synthetic CPU-reset state for unit tests.
    static GameBoySystem createForTest(std::vector<uint8_t> rom);

    // Versioned, portable snapshot at an M-cycle boundary. Throws on invalid
    // input; failed loads leave the machine and frontend memory pointers intact.
    static constexpr size_t MAX_STATE_SIZE = 512 * 1024;
    std::vector<uint8_t> saveState();
    void loadState(const void *data, size_t size);

    RetStepOneCycle stepOneCycle();
    RetStepFrame stepFrame();
    uint64_t getElapsedTCycles() const;
    Reg getCpuRegisters();
    const DmgPpu::FrameBuffer &getFrameBuffer() const;
    const DmgPpu::FrameBuffer &getCompletedFrameBuffer() const;
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
                  std::shared_ptr<std::vector<uint8_t>> saveRtc,
                  std::vector<uint8_t> dmgBootRom);
    void initializeDmgPostBootState();

    Cartridge cartridge;
    DmgBootRom dmgBootRom;
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
