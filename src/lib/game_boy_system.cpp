#include "include/game_boy_system.hpp"

#include <utility>
#include <stdexcept>

namespace LOTUSGB {

GameBoySystem::GameBoySystem(std::vector<uint8_t> rom)
    : GameBoySystem(std::move(rom), StartupProfile::DmgPostBoot, nullptr,
                    nullptr, {}) {}

GameBoySystem::GameBoySystem(
    std::vector<uint8_t> rom,
    std::shared_ptr<std::vector<uint8_t>> saveRam)
    : GameBoySystem(std::move(rom), StartupProfile::DmgPostBoot,
                    std::move(saveRam), nullptr, {}) {}

GameBoySystem::GameBoySystem(
    std::vector<uint8_t> rom,
    std::shared_ptr<std::vector<uint8_t>> saveRam,
    std::shared_ptr<std::vector<uint8_t>> saveRtc)
    : GameBoySystem(std::move(rom), StartupProfile::DmgPostBoot,
                    std::move(saveRam), std::move(saveRtc), {}) {}

GameBoySystem::GameBoySystem(
    std::vector<uint8_t> rom,
    std::shared_ptr<std::vector<uint8_t>> saveRam,
    std::shared_ptr<std::vector<uint8_t>> saveRtc,
    std::vector<uint8_t> dmgBootRom)
    : GameBoySystem(std::move(rom), StartupProfile::DmgPostBoot,
                    std::move(saveRam), std::move(saveRtc),
                    std::move(dmgBootRom)) {}

GameBoySystem::GameBoySystem(std::vector<uint8_t> rom, HardwareModel model,
    std::shared_ptr<std::vector<uint8_t>> saveRam,
    std::shared_ptr<std::vector<uint8_t>> saveRtc, std::vector<uint8_t> bootRom)
    : GameBoySystem(std::move(rom), model == HardwareModel::CgbE
          ? StartupProfile::CgbPostBoot : StartupProfile::DmgPostBoot,
          std::move(saveRam), std::move(saveRtc), std::move(bootRom), model) {}

GameBoySystem GameBoySystem::createForTest(std::vector<uint8_t> rom) {
    return GameBoySystem(std::move(rom), StartupProfile::TestReset, nullptr,
                         nullptr, {});
}

GameBoySystem GameBoySystem::createCgbBootProbe(std::vector<uint8_t> rom,
                                                std::vector<uint8_t> bootRom) {
    if (bootRom.empty())
        throw std::invalid_argument("CGB boot probe requires a boot fixture");
    if (rom.size() <= 0x143 || (rom[0x143] & 0x80) == 0)
        throw std::invalid_argument("Synthetic CGB boot probe requires a CGB-capable cartridge");
    return GameBoySystem(std::move(rom), StartupProfile::TestReset, nullptr,
                         nullptr, std::move(bootRom), HardwareModel::CgbE);
}

GameBoySystem::GameBoySystem(std::vector<uint8_t> rom,
                             StartupProfile startupProfile,
                             std::shared_ptr<std::vector<uint8_t>> saveRam,
                             std::shared_ptr<std::vector<uint8_t>> saveRtc,
                             std::vector<uint8_t> bootRomImage, HardwareModel model,
                             const std::array<uint8_t, 32> *knownRomHash)
    : model(model), cgbBootProbe(model == HardwareModel::CgbE && startupProfile == StartupProfile::TestReset),
      cartridge(std::move(rom), std::move(saveRam), std::move(saveRtc), {},
                knownRomHash),
      bootRom(cartridge, std::move(bootRomImage), model),
      cgbWorkRam(model == HardwareModel::CgbE ? std::make_unique<CgbWorkRam>() : nullptr),
      cgbVideoRam(model == HardwareModel::CgbE ? std::make_unique<CgbVideoRam>() : nullptr),
      cgbIo(model == HardwareModel::CgbE ? std::make_unique<CgbIo>(&bootRom) : nullptr),
      workRam(0xC000, 0x2000),
      highRam(0xFF80, 0x007F),
      joypad(interruptRegisters),
      serial(interruptRegisters, model == HardwareModel::CgbE),
      timer(interruptRegisters),
      apu(model == HardwareModel::CgbE),
      ppu(interruptRegisters, cgbVideoRam.get(), model == HardwareModel::CgbE),
      cgbVramDma(model == HardwareModel::CgbE ? std::make_unique<CgbVramDma>(bus, *cgbVideoRam, ppu) : nullptr),
      oamDma(bus, ppu, model == HardwareModel::CgbE),
      cpu(bus, decoder, decoderCB, &interruptRegisters),
      systemClock(cpu) {
    joypad.setStopWakeCallback([this] { cpu.wakeFromStop(); });
    bus.setCpuBusAccessCallback(
        [this] { systemClock.advanceToCpuBusAccess(); });
    bus.setCpuBusArbiter(oamDma);
    if (model == HardwareModel::Dmg)
        bus.setCpuBusEventCallback([this](uint16_t address, CpuBusEvent event) {
            ppu.onCpuBusAccess(address, event);
        });
    timer.setDividerApuClockCallback([this] { apu.onDividerApuFallingEdge(); });
    ppu.setFrameCompletedCallback([this] { completedFrameThisCycle = true; });
    ppu.setOamDmaActiveCallback([this] { return oamDma.isActive(); });
    ppu.setOamDmaPpuReadAddressCallback(
        [this](size_t address) { return oamDma.getPpuReadAddress(address); });

    bus.map(0x0000, 0x7FFF, bootRom);
    bus.map(0xA000, 0xBFFF, cartridge);
    IAddressRegion &activeWork = cgbWorkRam ? static_cast<IAddressRegion &>(*cgbWorkRam) : workRam;
    bus.map(0x8000, 0x9FFF, ppu);
    bus.map(0xC000, 0xDFFF, activeWork);
    bus.mapAlias(0xE000, 0xFDFF, activeWork, 0xC000);
    if (model == HardwareModel::CgbE) {
        const auto mapNative = [this](uint16_t first, uint16_t last, IAddressRegion &target) {
            cgbRegisterRegions.push_back(std::make_unique<CgbNativeRegisterRegion>(*cgbIo, target));
            bus.map(first, last, *cgbRegisterRegions.back());
        };
        bootRom.setUnmappedCallback([this] { applyCgbMode(); });
        bus.map(0xFF4C, 0xFF4C, *cgbIo);
        bus.map(0xFF6C, 0xFF6C, *cgbIo);
        mapNative(0xFF4F, 0xFF4F, *cgbVideoRam);
        mapNative(0xFF70, 0xFF70, *cgbWorkRam);
        bus.map(0xFF4D, 0xFF4D, *cgbIo);
        bus.map(0xFF56, 0xFF56, *cgbIo);
        bus.map(0xFF72, 0xFF75, *cgbIo);
        mapNative(0xFF76, 0xFF77, apu);
        mapNative(0xFF51, 0xFF55, *cgbVramDma);
        mapNative(0xFF68, 0xFF6B, ppu);
        ppu.setHBlankCallback([this] {
            // HALT pauses HBlank DMA without cancelling its remaining blocks.
            // The PPU keeps running; only a HBlank reached by an awake CPU
            // can begin a new block. An in-flight block already stalls the CPU.
            if (!cpu.getHALT()) cgbVramDma->onHBlank();
        });
        ppu.setLcdDisabledCallback([this] { cgbVramDma->onLcdDisabled(); });
    }
    bus.map(0xFE00, 0xFE9F, ppu);
    bus.map(0xFF80, 0xFFFE, highRam);
    bus.map(0xFF00, 0xFF00, joypad);
    bus.map(0xFF01, 0xFF02, serial);
    bus.map(0xFF04, 0xFF07, timer);
    bus.map(0xFF10, 0xFF26, apu);
    bus.map(0xFF30, 0xFF3F, apu);
    bus.map(0xFF40, 0xFF45, ppu);
    bus.map(0xFF47, 0xFF4B, ppu);
    bus.map(0xFF46, 0xFF46, oamDma);
    bus.map(0xFF50, 0xFF50, bootRom);
    bus.map(0xFF0F, 0xFF0F, interruptRegisters);
    bus.map(0xFFFF, 0xFFFF, interruptRegisters);
    systemClock.addClockedComponent(timer, true);
    systemClock.addClockedComponent(serial, true);
    systemClock.addClockedComponent(apu);
    systemClock.addClockedComponent(ppu);
    systemClock.addClockedComponent(cartridge);
    systemClock.addClockedComponent(oamDma, true);
    if (cgbVramDma) {
        systemClock.addClockedComponent(*cgbIo);
        systemClock.addClockedComponent(*cgbVramDma);
    }
    cpu.reset();
    if (bootRom.isMapped()) {
        cpu.setIME(false);
        if (model == HardwareModel::Dmg) {
            timer.initializeDmgBootRomState();
            serial.initializeDmgBootRomState();
            joypad.initializeDmgBootRomState();
        }
    } else if (startupProfile == StartupProfile::CgbPostBoot) {
        initializeCgbPostBootState();
    } else if (startupProfile == StartupProfile::DmgPostBoot) {
        initializeDmgPostBootState();
    }
}

bool GameBoySystem::getSerialOutgoingBit() const { return serial.getOutgoingBit(); }
void GameBoySystem::clockSerialExternalBit(bool incomingBit) { serial.clockExternalBit(incomingBit); }
void GameBoySystem::setSerialClockCallback(std::function<bool(bool)> callback) {
    serial.setInternalClockCallback(std::move(callback));
}
bool GameBoySystem::getInfraredLedState() const {
    return cgbIo && cgbIo->nativeFeaturesEnabled() && cgbIo->infraredLedOn();
}
void GameBoySystem::setInfraredInputCallback(std::function<bool()> callback) {
    if (!cgbIo) throw std::logic_error("CGB infrared input requires CGB hardware");
    cgbIo->setInfraredInputCallback(std::move(callback));
}

RetStepOneCycle GameBoySystem::stepOneCycle() {
    completedFrameThisCycle = false;
    if (cgbIo && cgbIo->speedPauseMCycles) {
        const auto result = systemClock.stepIdle(false);
        if (--cgbIo->speedPauseMCycles == 0) {
            cgbIo->doubleSpeed = !cgbIo->doubleSpeed;
            timer.setDoubleSpeed(cgbIo->doubleSpeed);
            systemClock.setDoubleSpeed(cgbIo->doubleSpeed);
            ppu.endSpeedSwitchPause();
            cpu.wakeFromStop();
        }
        return result;
    }
    if (cgbVramDma && cgbVramDma->blocksCpu()) return systemClock.stepIdle();
    const auto result = systemClock.step();
    if (cgbIo && cpu.getStopped() && cgbIo->switchArmed) {
        cgbIo->switchArmed = false;
        cgbIo->speedPauseMCycles = 2050;
        ppu.beginSpeedSwitchPause();
        timer.write(0xFF04, 0);
    }
    if (cgbBootProbe && (ppu.read(0xFF40) & 0x80))
        throw std::logic_error("Synthetic CGB boot probe is restricted to LCD off");
    return result;
}

RetStepFrame GameBoySystem::stepUntilVBlank(uint64_t maxTCycles) {
    if (maxTCycles == 0 || maxTCycles % T_CYCLES_PER_M_CYCLE != 0)
        throw std::invalid_argument("VBlank step budget must be a positive multiple of four T-cycles");
    RetStepFrame result;
    result.success = true;
    while (result.tCyclesElapsed < maxTCycles) {
        const auto step = stepOneCycle();
        result.tCyclesElapsed += step.tCyclesElapsed;
        if (!step.success) {
            result.success = false;
            return result;
        }
        if (completedFrameThisCycle) {
            result.frameCompleted = true;
            return result;
        }
        if (step.tCyclesElapsed == 0)
            return result;
    }
    return result;
}

RetPresentationStep GameBoySystem::stepForPresentation() {
    const auto step = stepUntilVBlank();
    const auto reason = !step.success ? PresentationReason::CpuFailure
        : step.frameCompleted ? PresentationReason::VBlank
        : step.tCyclesElapsed >= DMG_FRAME_T_CYCLES ? PresentationReason::Repeat
        : PresentationReason::CpuStopped;
    return {reason, step.tCyclesElapsed};
}

RetStepFrame GameBoySystem::stepFrame() {
    RetStepFrame result;
    result.success = true;

    while (result.tCyclesElapsed < DMG_FRAME_T_CYCLES) {
        const RetStepOneCycle step = stepOneCycle();
        if (!step.success) {
            result.success = false;
            return result;
        }
        if (step.tCyclesElapsed == 0)
            return result;

        result.tCyclesElapsed += step.tCyclesElapsed;
    }

    result.frameCompleted = true;
    return result;
}

uint64_t GameBoySystem::getElapsedTCycles() const {
    return systemClock.getElapsedTCycles();
}

Reg GameBoySystem::getCpuRegisters() {
    return cpu.getReg();
}

const DmgPpu::ColorFrameBuffer &GameBoySystem::getCompletedColorFrameBuffer() const {
    return ppu.getCompletedColorFrameBuffer();
}

const DmgPpu::FrameBuffer &GameBoySystem::getFrameBuffer() const {
    return ppu.getFrameBuffer();
}

const DmgPpu::FrameBuffer &GameBoySystem::getCompletedFrameBuffer() const {
    return ppu.getCompletedFrameBuffer();
}

std::vector<int16_t> GameBoySystem::takeAudioSamples() {
    return apu.takeAudioSamples();
}

void GameBoySystem::setButtonState(JoypadButton button, bool pressed) {
    joypad.setButtonState(button, pressed);
}

void GameBoySystem::setCartridgeAccelerometerData(uint16_t x, uint16_t y) {
    cartridge.setMbc7AccelerometerData(x, y);
}

bool GameBoySystem::isCartridgeRumbleActive() const {
    return cartridge.isRumbleActive();
}

void GameBoySystem::setCartridgeRumbleCallback(
    std::function<void(bool)> callback) {
    cartridge.setRumbleCallback(std::move(callback));
}

uint8_t *GameBoySystem::getSaveRamData() {
    if (!cartridge.hasBattery())
        return nullptr;
    return cartridge.getExternalRamData();
}

size_t GameBoySystem::getSaveRamSize() const {
    return cartridge.hasBattery() ? cartridge.getExternalRamSize() : 0;
}

std::shared_ptr<std::vector<uint8_t>>
GameBoySystem::getSaveRamStorage() const {
    return cartridge.getBatteryRamStorage();
}

uint8_t *GameBoySystem::getRtcData() {
    const auto rtc = cartridge.getBatteryRtcStorage();
    return rtc && !rtc->empty() ? rtc->data() : nullptr;
}

size_t GameBoySystem::getRtcSize() const {
    const auto rtc = cartridge.getBatteryRtcStorage();
    return rtc ? rtc->size() : 0;
}

std::shared_ptr<std::vector<uint8_t>> GameBoySystem::getRtcStorage() const {
    return cartridge.getBatteryRtcStorage();
}

void GameBoySystem::applyCgbMode() {
    if (!cgbIo) return;
    const bool compatibility = !cgbIo->nativeFeaturesEnabled();
    ppu.setDmgCompatibility(compatibility, cgbIo->xPriority());
    cgbVideoRam->setDmgCompatibility(compatibility);
    cgbWorkRam->setDmgCompatibility(compatibility);
    serial.setCgbFeaturesEnabled(!compatibility);
}

void GameBoySystem::initializeCgbPostBootState() {
    Reg r = cpu.getReg();
    r.getRefA() = 0x11; r.getRefF() = 0x80;
    r.getRefB() = 0; r.getRefC() = 0;
    r.getRefD() = 0xFF; r.getRefE() = 0x56;
    r.getRefH() = 0; r.getRefL() = 0x0D;
    r.getRefSP() = 0xFFFE; r.getRefPC() = 0x0100;
    cpu.setReg(r); cpu.setIME(false);
    timer.initializeCgbPostBootState();
    apu.initializeDmgPostBootState();
    ppu.initializeCgbPostBootState();
    if (!(cartridge.read(0x143) & 0x80)) {
        // Synthetic no-BIOS compatibility startup uses a neutral palette.
        // A supplied BIOS remains responsible for title/input colorization.
        for (auto index : {0xFF68, 0xFF6A}) {
            ppu.write(index, 0x80);
            for (unsigned palette = 0; palette < (index == 0xFF68 ? 1u : 2u); ++palette)
                for (uint16_t color : {0x7FFF, 0x56B5, 0x294A, 0}) {
                    ppu.write(index + 1, color & 255); ppu.write(index + 1, color >> 8);
                }
        }
        cgbIo->initializeCompatibility();
    }
    applyCgbMode();
    joypad.write(0xFF00, 0);
    interruptRegisters.write(0xFF0F, 0xE1);
    interruptRegisters.write(0xFFFF, 0);
}

void GameBoySystem::initializeDmgPostBootState() {
    Reg registers = cpu.getReg();
    const uint8_t headerChecksum = cartridge.read(0x014D);
    registers.getRefA() = 0x01;
    registers.getRefF() = headerChecksum == 0 ? 0x80 : 0xB0;
    registers.getRefB() = 0x00;
    registers.getRefC() = 0x13;
    registers.getRefD() = 0x00;
    registers.getRefE() = 0xD8;
    registers.getRefH() = 0x01;
    registers.getRefL() = 0x4D;
    registers.getRefSP() = 0xFFFE;
    registers.getRefPC() = 0x0100;
    cpu.setReg(registers);
    // The boot ROM hands control to the cartridge with interrupts disabled.
    cpu.setIME(false);

    timer.initializeDmgPostBootState();
    serial.initializeDmgPostBootState();
    apu.initializeDmgPostBootState();
    ppu.initializeDmgPostBootState();
    joypad.write(0xFF00, 0x00);
    interruptRegisters.write(0xFF0F, 0xE1);
    interruptRegisters.write(0xFFFF, 0x00);
}

}
