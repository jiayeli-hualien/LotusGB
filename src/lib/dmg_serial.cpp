#include "include/dmg_serial.hpp"

#include <algorithm>
#include <utility>

namespace LOTUSGB {

namespace {
constexpr uint16_t SB_ADDRESS = 0xFF01;
constexpr uint16_t SC_ADDRESS = 0xFF02;
constexpr uint8_t TRANSFER_ENABLE = 0x80;
constexpr uint8_t INTERNAL_CLOCK = 0x01;
constexpr uint8_t UNUSED_SC_BITS = 0x7E;
constexpr unsigned BIT_PERIOD_T_CYCLES = 512; // 4,194,304 / 8,192 Hz
constexpr unsigned DMG_POST_BOOT_SERIAL_PHASE_T_CYCLES = 0x1C8;
constexpr unsigned DMG_BOOT_ROM_SERIAL_PHASE_T_CYCLES = 4;
}

DmgSerial::DmgSerial(IInterruptController &interrupts, bool cgbMode)
    : interrupts(interrupts), cgbMode(cgbMode) {}

void DmgSerial::initializeDmgPostBootState() {
    // The synthesized DIV handoff value is $ABC8; the serial divider's
    // corresponding phase is the low nine bits, $1C8.
    clockPhaseTCycles = DMG_POST_BOOT_SERIAL_PHASE_T_CYCLES;
}

void DmgSerial::initializeDmgBootRomState() {
    // CPU execution begins four T-cycles after reset, so the free-running
    // serial divider has already advanced by four cycles at the first fetch.
    clockPhaseTCycles = DMG_BOOT_ROM_SERIAL_PHASE_T_CYCLES;
}

uint8_t DmgSerial::read(uint16_t address) {
    if (address == SB_ADDRESS)
        return data;
    if (address == SC_ADDRESS)
        return (control & (cgbMode ? 0x83 : 0x81)) | (cgbMode ? 0x7C : UNUSED_SC_BITS);
    return 0xFF;
}

void DmgSerial::write(uint16_t address, uint8_t value) {
    if (address == SB_ADDRESS) {
        data = value;
    } else if (address == SC_ADDRESS) {
        control = value & (TRANSFER_ENABLE | INTERNAL_CLOCK | (cgbMode ? 2 : 0));
        bitsTransferred = 0;
        // SC requests a transfer, but does not restart the clock divider.
    }
}

void DmgSerial::advanceTCycles(uint64_t tCycles) {
    const unsigned period = cgbMode && (control & 2) ? 16 : BIT_PERIOD_T_CYCLES;
    clockPhaseTCycles %= period;
    while (tCycles > 0) {
        const auto elapsed = std::min<uint64_t>(
            tCycles, period - clockPhaseTCycles);
        clockPhaseTCycles += static_cast<unsigned>(elapsed);
        tCycles -= elapsed;
        if (clockPhaseTCycles == period) {
            clockPhaseTCycles = 0;
            clockBit();
        }
    }
}

bool DmgSerial::getOutgoingBit() const { return (data & 0x80) != 0; }

void DmgSerial::setInternalClockCallback(std::function<bool(bool)> callback) {
    internalClockCallback = std::move(callback);
}

void DmgSerial::clockExternalBit(bool incomingBit) {
    if ((control & (TRANSFER_ENABLE | INTERNAL_CLOCK)) == TRANSFER_ENABLE)
        shiftBit(incomingBit);
}

void DmgSerial::clockBit() {
    if ((control & (TRANSFER_ENABLE | INTERNAL_CLOCK)) !=
        (TRANSFER_ENABLE | INTERNAL_CLOCK))
        return;

    // Both outgoing bits must be sampled before either peer shifts. The host
    // callback can sample a peer then deliver this outgoing bit to that peer.
    const bool incoming = internalClockCallback ? internalClockCallback(getOutgoingBit()) : true;
    shiftBit(incoming);
}

void DmgSerial::shiftBit(bool incomingBit) {
    data = static_cast<uint8_t>((data << 1) | (incomingBit ? 1 : 0));
    if (++bitsTransferred == 8) {
        control &= static_cast<uint8_t>(~TRANSFER_ENABLE);
        bitsTransferred = 0;
        interrupts.requestInterrupt(INTERRUPT_SERIAL);
    }
}

}
