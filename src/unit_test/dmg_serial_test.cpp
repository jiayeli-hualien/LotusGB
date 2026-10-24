#include <cstdlib>
#include <iostream>

#include "dmg_serial.hpp"
#include "interrupt_registers.hpp"

namespace {
using namespace LOTUSGB;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void testRegistersAndDisconnectedTransfer() {
    InterruptRegisters interrupts;
    DmgSerial serial(interrupts);
    expect(serial.read(0xFF01) == 0 && serial.read(0xFF02) == 0x7E,
           "serial starts idle with DMG unused SC bits high");
    serial.write(0xFF01, 0);
    serial.write(0xFF02, 0xFF);
    expect(serial.read(0xFF02) == 0xFF, "only SC transfer/clock bits are writable on DMG");
    for (unsigned bit = 0; bit < 8; ++bit) {
        const uint8_t previous = static_cast<uint8_t>((1U << bit) - 1);
        serial.advanceTCycles(511);
        expect(serial.read(0xFF01) == previous, "SB stays unchanged before a serial clock edge");
        expect((serial.read(0xFF02) & 0x80) != 0, "transfer remains active before the eighth edge");
        expect((interrupts.read(0xFF0F) & 8) == 0, "serial IRQ waits for the complete byte");
        serial.advanceTCycles(1);
        expect(serial.read(0xFF01) == static_cast<uint8_t>((previous << 1) | 1),
               "each edge shifts in one disconnected high bit");
    }
    expect(serial.read(0xFF01) == 0xFF && serial.read(0xFF02) == 0x7F,
           "eight bits receive FF and clear SC transfer enable");
    expect((interrupts.read(0xFF0F) & 8) != 0,
           "serial completion requests IF even with IE disabled");
    interrupts.write(0xFF0F, 0);
    serial.advanceTCycles(8192);
    expect((interrupts.read(0xFF0F) & 8) == 0, "completed transfer does not repeat its IRQ");
}

void testExternalClockWaitsAndAbortRetainsPartialData() {
    InterruptRegisters interrupts;
    DmgSerial serial(interrupts);
    serial.write(0xFF01, 0xA5);
    serial.write(0xFF02, 0x80);
    serial.advanceTCycles(16384);
    expect(serial.read(0xFF01) == 0xA5 && serial.read(0xFF02) == 0xFE,
           "external mode waits indefinitely without a connected clock");
    expect((interrupts.read(0xFF0F) & 8) == 0, "waiting for an external clock does not request IRQ");
    serial.write(0xFF02, 0x81);
    serial.advanceTCycles(512 * 3);
    expect(serial.read(0xFF01) == 0x2F, "SB exposes partially shifted data");
    serial.write(0xFF02, 1);
    serial.advanceTCycles(8192);
    expect(serial.read(0xFF01) == 0x2F && (interrupts.read(0xFF0F) & 8) == 0,
           "clearing transfer enable stops shifting without completion IRQ");
    serial.write(0xFF02, 0x81);
    serial.advanceTCycles(512 * 7);
    expect((serial.read(0xFF02) & 0x80) != 0, "a new transfer starts a fresh eight-bit count");
    serial.advanceTCycles(512);
    expect(serial.read(0xFF01) == 0xFF && (interrupts.read(0xFF0F) & 8),
           "a transfer can complete after a previous transfer was aborted");
}

void testStartDoesNotResetSerialClockPhase() {
    InterruptRegisters interrupts;
    DmgSerial serial(interrupts);
    serial.advanceTCycles(120);
    serial.write(0xFF01, 0x80);
    serial.write(0xFF02, 0x81);
    serial.advanceTCycles(391);
    expect(serial.read(0xFF01) == 0x80, "first bit waits for the existing clock phase");
    serial.advanceTCycles(1);
    expect(serial.read(0xFF01) == 1, "SC write does not restart the free-running serial clock");
    serial.advanceTCycles(512 * 7);
    expect(serial.read(0xFF02) == 0x7F && (interrupts.read(0xFF0F) & 8),
           "completion follows eight shared-clock edges rather than a new 4096-cycle countdown");
}
}

int main() {
    testRegistersAndDisconnectedTransfer();
    testExternalClockWaitsAndAbortRetainsPartialData();
    testStartDoesNotResetSerialClockPhase();
    std::cout << "DMG serial tests passed\n";
}
