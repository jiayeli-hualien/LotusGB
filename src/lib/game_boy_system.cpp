#include "include/game_boy_system.hpp"

#include <utility>

namespace LOTUSGB {

GameBoySystem::GameBoySystem(std::vector<uint8_t> rom)
    : cartridge(std::move(rom)),
      videoRam(0x8000, 0x2000),
      workRam(0xC000, 0x2000),
      objectRam(0xFE00, 0x00A0),
      highRam(0xFF80, 0x007F),
      timer(interruptRegisters),
      cpu(bus, decoder, decoderCB, &interruptRegisters),
      systemClock(cpu) {
    bus.map(0x0000, 0x7FFF, cartridge);
    bus.map(0x8000, 0x9FFF, videoRam);
    bus.map(0xC000, 0xDFFF, workRam);
    bus.map(0xFE00, 0xFE9F, objectRam);
    bus.map(0xFF80, 0xFFFE, highRam);
    bus.map(0xFF04, 0xFF07, timer);
    bus.map(0xFF0F, 0xFF0F, interruptRegisters);
    bus.map(0xFFFF, 0xFFFF, interruptRegisters);
    systemClock.addClockedComponent(timer);
    cpu.reset();
}

RetStepOneCycle GameBoySystem::stepOneCycle() {
    return systemClock.step();
}

uint64_t GameBoySystem::getElapsedTCycles() const {
    return systemClock.getElapsedTCycles();
}

Reg GameBoySystem::getCpuRegisters() {
    return cpu.getReg();
}

}
