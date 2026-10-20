#include "include/game_boy_system.hpp"

#include <utility>

namespace LOTUSGB {

GameBoySystem::GameBoySystem(std::vector<uint8_t> rom,
                             SystemStartupMode startupMode)
    : cartridge(std::move(rom)),
      workRam(0xC000, 0x2000),
      highRam(0xFF80, 0x007F),
      joypad(interruptRegisters),
      timer(interruptRegisters),
      ppu(interruptRegisters),
      cpu(bus, decoder, decoderCB, &interruptRegisters),
      systemClock(cpu) {
    bus.map(0x0000, 0x7FFF, cartridge);
    bus.map(0x8000, 0x9FFF, ppu);
    bus.map(0xC000, 0xDFFF, workRam);
    bus.map(0xFE00, 0xFE9F, ppu);
    bus.map(0xFF80, 0xFFFE, highRam);
    bus.map(0xFF00, 0xFF00, joypad);
    bus.map(0xFF04, 0xFF07, timer);
    bus.map(0xFF40, 0xFF45, ppu);
    bus.map(0xFF47, 0xFF4B, ppu);
    bus.map(0xFF0F, 0xFF0F, interruptRegisters);
    bus.map(0xFFFF, 0xFFFF, interruptRegisters);
    systemClock.addClockedComponent(timer);
    systemClock.addClockedComponent(ppu);
    cpu.reset();
    if (startupMode == SystemStartupMode::DmgPostBoot)
        initializeDmgPostBootState();
}

RetStepOneCycle GameBoySystem::stepOneCycle() {
    return systemClock.step();
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

const DmgPpu::FrameBuffer &GameBoySystem::getFrameBuffer() const {
    return ppu.getFrameBuffer();
}

void GameBoySystem::setButtonState(JoypadButton button, bool pressed) {
    joypad.setButtonState(button, pressed);
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
    cpu.setIME(true);

    timer.initializeDmgPostBootState();
    ppu.initializeDmgPostBootState();
    joypad.write(0xFF00, 0x00);
    interruptRegisters.write(0xFF0F, 0xE1);
    interruptRegisters.write(0xFFFF, 0x00);
}

}
