#include "include/dmg_serial.hpp"

#include <algorithm>

namespace LOTUSGB {

namespace {
constexpr uint16_t SB_ADDRESS = 0xFF01;
constexpr uint16_t SC_ADDRESS = 0xFF02;
constexpr uint8_t TRANSFER_ENABLE = 0x80;
constexpr uint8_t INTERNAL_CLOCK = 0x01;
constexpr uint8_t UNUSED_SC_BITS = 0x7E;
constexpr unsigned BIT_PERIOD_T_CYCLES = 512; // 4,194,304 / 8,192 Hz
}

DmgSerial::DmgSerial(IInterruptController &interrupts) : interrupts(interrupts) {}

uint8_t DmgSerial::read(uint16_t address) {
    if (address == SB_ADDRESS)
        return data;
    if (address == SC_ADDRESS)
        return control | UNUSED_SC_BITS;
    return 0xFF;
}

void DmgSerial::write(uint16_t address, uint8_t value) {
    if (address == SB_ADDRESS) {
        data = value;
    } else if (address == SC_ADDRESS) {
        control = value & (TRANSFER_ENABLE | INTERNAL_CLOCK);
        bitsTransferred = 0;
        // SC requests a transfer, but does not restart the clock divider.
    }
}

void DmgSerial::advanceTCycles(uint64_t tCycles) {
    while (tCycles > 0) {
        const auto elapsed = std::min<uint64_t>(
            tCycles, BIT_PERIOD_T_CYCLES - clockPhaseTCycles);
        clockPhaseTCycles += static_cast<unsigned>(elapsed);
        tCycles -= elapsed;
        if (clockPhaseTCycles == BIT_PERIOD_T_CYCLES) {
            clockPhaseTCycles = 0;
            clockBit();
        }
    }
}

void DmgSerial::clockBit() {
    if ((control & (TRANSFER_ENABLE | INTERNAL_CLOCK)) !=
        (TRANSFER_ENABLE | INTERNAL_CLOCK))
        return;

    // With no peer connected the incoming line is pulled high.
    data = static_cast<uint8_t>((data << 1) | 1);
    if (++bitsTransferred == 8) {
        control &= static_cast<uint8_t>(~TRANSFER_ENABLE);
        bitsTransferred = 0;
        interrupts.requestInterrupt(INTERRUPT_SERIAL);
    }
}

}
