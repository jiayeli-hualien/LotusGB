#include <cstdlib>
#include <iostream>
#include <stdexcept>

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

void testExternalEdgesRequireActiveExternalClock() {
    InterruptRegisters interrupts;
    DmgSerial serial(interrupts);
    serial.write(0xFF01, 0xA5);
    serial.write(0xFF02, 0x81);
    serial.clockExternalBit(false);
    expect(serial.read(0xFF01) == 0xA5 && serial.getBitsTransferred() == 0,
           "external edge is ignored while an internal-clock transfer is active");
    serial.write(0xFF02, 0x80);
    serial.clockExternalBit(false);
    expect(serial.read(0xFF01) == 0x4A && serial.getBitsTransferred() == 1,
           "active external-clock transfer accepts the supplied edge");
    serial.write(0xFF02, 0x00);
    serial.clockExternalBit(true);
    expect(serial.read(0xFF01) == 0x4A && serial.getBitsTransferred() == 0,
           "external edge is ignored after transfer-start is cleared");
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

void testStartupProfilesRestoreSerialClockPhase() {
    {
        InterruptRegisters interrupts;
        DmgSerial serial(interrupts);
        serial.initializeDmgBootRomState();
        serial.write(0xFF01, 0x80);
        serial.write(0xFF02, 0x81);
        serial.advanceTCycles(507);
        expect(serial.read(0xFF01) == 0x80,
               "boot-ROM startup phase has not reached its first serial edge at 507 cycles");
        serial.advanceTCycles(1);
        expect(serial.read(0xFF01) == 1,
               "boot-ROM serial phase starts four T-cycles after reset");
    }

    {
        InterruptRegisters interrupts;
        DmgSerial serial(interrupts);
        serial.initializeDmgPostBootState();
        serial.write(0xFF01, 0x80);
        serial.write(0xFF02, 0x81);
        serial.advanceTCycles(55);
        expect(serial.read(0xFF01) == 0x80,
               "post-boot startup phase has not reached its first serial edge at 55 cycles");
        serial.advanceTCycles(1);
        expect(serial.read(0xFF01) == 1,
               "post-boot serial phase matches the synthesized $ABC8 divider phase");
    }
}
void testExternalEdgesAndTwoPortExchange() {
    for (bool cgbMaster : {false, true}) {
        for (bool cgbSlave : {false, true}) {
            for (bool fast : {false, true}) {
                InterruptRegisters masterIrq, slaveIrq;
                DmgSerial master(masterIrq, cgbMaster), slave(slaveIrq, cgbSlave);
                master.write(0xFF01, 0xA5); slave.write(0xFF01, 0x3C);
                slave.write(0xFF02, 0x80);
                unsigned edges = 0;
                master.setInternalClockCallback([&](bool outgoing) {
                    const bool incoming = slave.getOutgoingBit();
                    slave.clockExternalBit(outgoing);
                    ++edges;
                    return incoming;
                });
                master.write(0xFF02, fast ? 0x83 : 0x81);
                const unsigned period = cgbMaster && fast ? 16 : 512;
                for (unsigned bit = 0; bit < 8; ++bit) {
                    slave.advanceTCycles(97 + bit * 31); // Slave's time never supplies external edges.
                    master.advanceTCycles(period - 1);
                    expect(edges == bit, "paired link shifts only on master clock edges");
                    master.advanceTCycles(1);
                    const uint8_t mask = static_cast<uint8_t>((1u << (bit + 1)) - 1);
                    const uint8_t expectedMaster = static_cast<uint8_t>((0xA5 << (bit + 1)) | ((0x3C >> (7 - bit)) & mask));
                    const uint8_t expectedSlave = static_cast<uint8_t>((0x3C << (bit + 1)) | ((0xA5 >> (7 - bit)) & mask));
                    expect(master.read(0xFF01) == expectedMaster && slave.read(0xFF01) == expectedSlave,
                           "both ports sample outgoing MSBs before shifting partial received data");
                    expect(master.getBitsTransferred() == ((bit + 1) % 8) &&
                               slave.getBitsTransferred() == ((bit + 1) % 8),
                           "serial diagnostic bit counts track the active byte on both ports");
                    expect(bool(masterIrq.read(0xFF0F) & 8) == (bit == 7) &&
                               bool(slaveIrq.read(0xFF0F) & 8) == (bit == 7),
                           "both serial IRQs occur only after the eighth edge");
                }
                master.advanceTCycles(period * 10);
                slave.clockExternalBit(false);
                expect(edges == 8 && master.read(0xFF01) == 0x3C && slave.read(0xFF01) == 0xA5,
                       "completed link transfers stop shifting and exchange complete bytes");
            }
        }
    }
}

void testChallengeResponseDistinguishesPeerFromDisconnectedLine() {
    constexpr uint8_t CHALLENGE = 0xC3;
    constexpr uint8_t ACKNOWLEDGEMENT = 0x5A;

    {
        InterruptRegisters interrupts;
        DmgSerial probe(interrupts);
        probe.write(0xFF01, CHALLENGE);
        probe.write(0xFF02, 0x81);
        probe.advanceTCycles(8 * 512);
        expect(probe.read(0xFF01) != ACKNOWLEDGEMENT &&
                   probe.read(0xFF01) == 0xFF,
               "a disconnected serial line cannot produce the probe acknowledgement");
    }

    {
        InterruptRegisters probeInterrupts, peerInterrupts;
        DmgSerial probe(probeInterrupts), peer(peerInterrupts);
        probe.write(0xFF01, CHALLENGE);
        probe.write(0xFF02, 0x81);
        peer.write(0xFF01, ACKNOWLEDGEMENT);
        peer.write(0xFF02, 0x80);
        probe.setInternalClockCallback([&](bool outgoing) {
            const bool incoming = peer.getOutgoingBit();
            peer.clockExternalBit(outgoing);
            return incoming;
        });
        probe.advanceTCycles(8 * 512);

        expect(probe.read(0xFF01) == ACKNOWLEDGEMENT &&
                   peer.read(0xFF01) == CHALLENGE,
               "a peer returning the expected acknowledgement completes the probe");
        expect((probeInterrupts.read(0xFF0F) & 8) != 0 &&
                   (peerInterrupts.read(0xFF0F) & 8) != 0,
               "both probe endpoints signal completion after the full response byte");
    }
}

void testIrregularExternalEdgesAbortAndModeGate() {
    InterruptRegisters irq;
    DmgSerial serial(irq, true);
    serial.write(0xFF01, 0xA5);
    serial.clockExternalBit(false);
    expect(serial.read(0xFF01) == 0xA5, "idle port ignores external edges");
    serial.write(0xFF02, 0x82); // Fast select does not turn external mode into an internal clock.
    for (unsigned bit = 0; bit < 3; ++bit) {
        serial.advanceTCycles(1001 + bit * 12345);
        serial.clockExternalBit(false);
    }
    expect(serial.read(0xFF01) == 0x28 && !(irq.read(0xFF0F) & 8),
           "irregular external edges expose partial data without an early IRQ");
    serial.write(0xFF02, 0); serial.clockExternalBit(true);
    expect(serial.read(0xFF01) == 0x28, "abort keeps partial data and ignores subsequent edges");
    serial.write(0xFF02, 0x80);
    for (unsigned bit = 0; bit < 7; ++bit) serial.clockExternalBit(true);
    expect(serial.read(0xFF02) & 0x80, "restarted external transfer requires a fresh eight edges");
    serial.clockExternalBit(true);
    expect(serial.read(0xFF01) == 0xFF && (irq.read(0xFF0F) & 8), "restarted external transfer completes normally");
    serial.write(0xFF01, 0x55); serial.write(0xFF02, 0x81);
    serial.clockExternalBit(false);
    expect(serial.read(0xFF01) == 0x55, "externally supplied edges do not clock internal mode");
}

void testSerialCallbackFailureAndDisconnect() {
    InterruptRegisters irq;
    DmgSerial serial(irq);
    serial.write(0xFF01, 0xA5); serial.write(0xFF02, 0x81);
    serial.setInternalClockCallback([](bool) -> bool { throw std::runtime_error("link failed"); });
    bool failed = false;
    try { serial.advanceTCycles(512); } catch (const std::runtime_error &) { failed = true; }
    expect(failed && serial.read(0xFF01) == 0xA5 && !(irq.read(0xFF0F) & 8),
           "link callback failures propagate without inventing a received bit or successful IRQ");
    serial.setInternalClockCallback({});
    serial.advanceTCycles(4096);
    expect(serial.read(0xFF01) == 0xFF && (irq.read(0xFF0F) & 8),
           "removing link callback restores documented disconnected incoming-high fallback");
}

}

int main() {
    testSerialCallbackFailureAndDisconnect();
    testExternalEdgesAndTwoPortExchange();
    testChallengeResponseDistinguishesPeerFromDisconnectedLine();
    testIrregularExternalEdgesAbortAndModeGate();
    testRegistersAndDisconnectedTransfer();
    testExternalClockWaitsAndAbortRetainsPartialData();
    testExternalEdgesRequireActiveExternalClock();
    testStartDoesNotResetSerialClockPhase();
    testStartupProfilesRestoreSerialClockPhase();
    std::cout << "DMG serial tests passed\n";
}
