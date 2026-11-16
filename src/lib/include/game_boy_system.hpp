#ifndef LOTUSGB_GAME_BOY_SYSTEM_HPP
#define LOTUSGB_GAME_BOY_SYSTEM_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "GBCPU.hpp"
#include "address_bus.hpp"
#include "cartridge.hpp"
#include "decoder.hpp"
#include "dmg_apu.hpp"
#include "boot_rom.hpp"
#include "cgb_memory.hpp"
#include "cgb_hardware.hpp"
#include "dmg_ppu.hpp"
#include "dmg_serial.hpp"
#include "dmg_oam_dma.hpp"
#include "interrupt_registers.hpp"
#include "joypad.hpp"
#include "memory_region.hpp"
#include "system_clock.hpp"
#include "timer.hpp"

namespace LOTUSGB {

namespace Test {
struct GameBoySystemTestAccess;
}

struct RetStepFrame {
    bool success = false;
    bool frameCompleted = false;
    uint64_t tCyclesElapsed = 0;
};

enum class PresentationReason { VBlank, Repeat, CpuStopped, CpuFailure };

struct RetPresentationStep {
    PresentationReason reason = PresentationReason::CpuFailure;
    uint64_t tCyclesElapsed = 0;
};

struct SerialDiagnosticState {
    uint64_t elapsedTCycles = 0;
    uint16_t programCounter = 0;
    uint8_t serialData = 0xFF;
    uint8_t serialControl = 0xFF;
    uint8_t interruptFlags = 0xFF;
    uint8_t interruptEnable = 0xFF;
    bool interruptMasterEnable = false;
    bool halted = false;
    bool stopped = false;
};

// Shared DMG/native-CGB system composition.
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
    // Explicit physical hardware selection; libretro selects by header bit 7.
    GameBoySystem(std::vector<uint8_t> rom, HardwareModel model,
                  std::shared_ptr<std::vector<uint8_t>> saveRam = nullptr,
                  std::shared_ptr<std::vector<uint8_t>> saveRtc = nullptr,
                  std::vector<uint8_t> bootRom = {});
    const DmgPpu::ColorFrameBuffer &getCompletedColorFrameBuffer() const;
    // Build a system from the synthetic CPU-reset state for unit tests.
    static GameBoySystem createForTest(std::vector<uint8_t> rom);

    // Synthetic LCD-off boot fixture composition for mapping/handoff tests.
    // Requires a CGB-capable cartridge and an address-indexed boot fixture.
    // This test-only entry point rejects LCD enable and snapshots.
    static GameBoySystem createCgbBootProbe(std::vector<uint8_t> rom,
                                            std::vector<uint8_t> bootRom);
    HardwareModel getHardwareModel() const { return model; }
    bool isDmgCompatibilityMode() const { return cgbIo && !cgbIo->nativeFeaturesEnabled(); }

    // Versioned, portable snapshot at an M-cycle boundary. Throws on invalid
    // input; failed loads leave the machine and frontend memory pointers intact.
    static constexpr size_t MAX_STATE_SIZE = 512 * 1024;
    std::vector<uint8_t> saveState();
    void loadState(const void *data, size_t size);

    RetStepOneCycle stepOneCycle();
    unsigned baseClocksPerMCycle() const { return systemClock.baseClocksPerMCycle(); }
    RetStepFrame stepFrame();
    // Diagnostic stepping to the next actual PPU frame-completion event,
    // including when already in VBlank. Stops on CPU failure/STOP or budget
    // exhaustion (e.g. LCD off); frameCompleted is false if no event occurred.
    // Advances whole M-cycles; maxTCycles must be a positive multiple of four.
    RetStepFrame stepUntilVBlank(uint64_t maxTCycles = DMG_FRAME_T_CYCLES);
    // Frontend return policy: a real complete frame, or a bounded repeat when
    // no frame arrives. Repeat is a host event; it never requests a guest IRQ.
    RetPresentationStep stepForPresentation();
    uint64_t getElapsedTCycles() const;
    Reg getCpuRegisters();
    SerialDiagnosticState getSerialDiagnosticState();
    const DmgPpu::FrameBuffer &getFrameBuffer() const;
    const DmgPpu::FrameBuffer &getCompletedFrameBuffer() const;
    std::vector<int16_t> takeAudioSamples();
    void setButtonState(JoypadButton button, bool pressed);
    // Raw MBC7 X/Y register values; the cartridge exposes each new pair only
    // after the guest performs its documented latch sequence.
    void setCartridgeAccelerometerData(uint16_t x, uint16_t y);
    bool isCartridgeRumbleActive() const;
    void setCartridgeRumbleCallback(std::function<void(bool)> callback);
    // Link hosts schedule sampling edges and wire peers; callbacks are not serialized.
    uint8_t getSerialData() const;
    bool getSerialOutgoingBit() const;
    unsigned getSerialBitsTransferred() const;
    uint8_t getSerialControl();
    void clockSerialExternalBit(bool incomingBit);
    bool completeSerialNetworkByte(uint8_t incomingByte);
    void setSerialClockCallback(
        std::function<std::optional<bool>(bool)> callback);
    // CGB IR is level-based: the callback reports whether an external light
    // source is detected when the guest samples FF56. Host wiring is not state.
    bool getInfraredLedState() const;
    void setInfraredInputCallback(std::function<bool()> callback);
    uint8_t *getSaveRamData();
    size_t getSaveRamSize() const;
    std::shared_ptr<std::vector<uint8_t>> getSaveRamStorage() const;
    uint8_t *getRtcData();
    size_t getRtcSize() const;
    std::shared_ptr<std::vector<uint8_t>> getRtcStorage() const;

private:
    friend struct Test::GameBoySystemTestAccess;

    enum class StartupProfile {
        TestReset,
        DmgPostBoot,
        CgbPostBoot,
    };

    GameBoySystem(std::vector<uint8_t> rom, StartupProfile startupProfile,
                  std::shared_ptr<std::vector<uint8_t>> saveRam,
                  std::shared_ptr<std::vector<uint8_t>> saveRtc,
                  std::vector<uint8_t> bootRom,
                  HardwareModel model = HardwareModel::Dmg,
                  const std::array<uint8_t, 32> *knownRomHash = nullptr);
    void initializeDmgPostBootState();
    void initializeCgbPostBootState();
    void applyCgbMode();

    HardwareModel model;
    bool cgbBootProbe;
    Cartridge cartridge;
    BootRom bootRom;
    std::unique_ptr<CgbWorkRam> cgbWorkRam;
    std::unique_ptr<CgbVideoRam> cgbVideoRam;
    std::unique_ptr<CgbIo> cgbIo;
    std::vector<std::unique_ptr<CgbNativeRegisterRegion>> cgbRegisterRegions;
    RamRegion workRam;
    RamRegion highRam;
    InterruptRegisters interruptRegisters;
    Joypad joypad;
    DmgSerial serial;
    Timer timer;
    DmgApu apu;
    DmgPpu ppu;
    AddressBus bus;
    std::unique_ptr<CgbVramDma> cgbVramDma;
    DmgOamDma oamDma;
    Decoder decoder{false};
    Decoder decoderCB{true};
    GBCPU cpu;
    SystemClock systemClock;
    // Ephemeral host notification, reset before every M-cycle, not save data.
    bool completedFrameThisCycle = false;
};

}

#endif // LOTUSGB_GAME_BOY_SYSTEM_HPP
