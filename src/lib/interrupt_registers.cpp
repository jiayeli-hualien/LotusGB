#include "include/interrupt_registers.hpp"

namespace LOTUSGB {

namespace {
constexpr uint16_t INTERRUPT_FLAGS_ADDRESS = 0xFF0F;
constexpr uint16_t INTERRUPT_ENABLE_ADDRESS = 0xFFFF;
constexpr uint8_t INTERRUPT_MASK = 0x1F;
}

uint8_t InterruptRegisters::read(uint16_t address) {
    if (address == INTERRUPT_FLAGS_ADDRESS)
        return static_cast<uint8_t>(0xE0 | interruptFlags);
    if (address == INTERRUPT_ENABLE_ADDRESS)
        return interruptEnable;
    return 0xFF;
}

void InterruptRegisters::write(uint16_t address, uint8_t value) {
    if (address == INTERRUPT_FLAGS_ADDRESS)
        interruptFlags = value & INTERRUPT_MASK;
    else if (address == INTERRUPT_ENABLE_ADDRESS)
        interruptEnable = value;
}

void InterruptRegisters::requestTimerInterrupt() {
    requestInterrupt(0x04);
}

uint8_t InterruptRegisters::getPendingInterrupts() const {
    return interruptFlags & interruptEnable;
}

void InterruptRegisters::acknowledgeInterrupt(uint8_t interruptMask) {
    interruptFlags &= static_cast<uint8_t>(~interruptMask);
}

void InterruptRegisters::requestInterrupt(uint8_t interruptMask) {
    interruptFlags |= interruptMask & INTERRUPT_MASK;
}

}
