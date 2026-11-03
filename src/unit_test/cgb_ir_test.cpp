#include <cstdlib>
#include <iostream>
#include <vector>

#include "cgb_hardware.hpp"
#include "game_boy_system.hpp"

namespace {
using namespace LOTUSGB;

void expect(bool ok, const char *message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

void testRegisterAndReceiverWarmup() {
    CgbIo port;
    bool light = true;
    unsigned samples = 0;
    port.setInfraredInputCallback([&] { ++samples; return light; });
    expect(port.read(0xFF56) == 0x3E, "disabled CGB IR receiver reads inactive with unused bits high");
    port.write(0xFF56, 0xFF);
    expect(port.read(0xFF56) == 0xFF && samples == 0,
           "FF56 write controls only receive enable and LED while the sensor warms up");
    port.advanceTCycles(CgbIo::IR_RECEIVER_SETTLE_T_CYCLES - 4);
    expect(port.read(0xFF56) == 0xFF && samples == 0,
           "receiver does not sample before the full 50 ms settling interval");
    port.write(0xFF56, 0xC0); // Changing the LED does not restart receiver settling.
    port.advanceTCycles(4);
    expect(port.read(0xFF56) == 0xFC && samples == 1,
           "enabled receiver reports active-low input after settling");
    port.write(0xFF56, 0xC1);
    expect(port.read(0xFF56) == 0xFD && samples == 1,
           "LED output and active-low input are independently observable");
    light = false;
    expect(port.read(0xFF56) == 0xFD && samples == 1,
           "RP reads the last system-clock sample rather than querying the sensor directly");
    port.advanceTCycles(4);
    expect(port.read(0xFF56) == 0xFF && samples == 2,
           "the next system-clock sample latches a dark input as the normal high sensor bit");
    port.write(0xFF56, 0);
    expect(port.read(0xFF56) == 0x3E && samples == 2,
           "disabling receive masks the sensor and preserves read-only bit semantics");
    port.write(0xFF56, 0xC0);
    expect(port.read(0xFF56) == 0xFE && samples == 2,
           "re-enabling receive starts a new settling interval");
}

std::vector<uint8_t> makeIrGuest(bool emitter, bool doubleSpeed) {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x143] = 0xC0; // Original CGB-only guest fixture.
    rom[0x147] = 0x03; // MBC1 + RAM + battery.
    rom[0x149] = 0x02; // 8 KiB SRAM for CPU-observed results.
    size_t at = 0x150;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t byte : bytes) rom[at++] = byte;
    };
    emit({0xF3, 0x31, 0xFE, 0xFF}); // DI; SP = FFFE.
    if (!emitter) emit({0x3E, 0x0A, 0xEA, 0x00, 0x00}); // Enable SRAM.
    if (doubleSpeed) emit({0x3E, 0x01, 0xE0, 0x4D, 0x10, 0x00}); // KEY1; STOP.
    if (emitter) {
        emit({0x3E, 0xC1, 0xE0, 0x56, 0x18, 0xFE}); // Enable IR and LED; hold light.
    } else {
        emit({0x3E, 0xC0, 0xE0, 0x56}); // Enable receiver, LED off.
        emit({0x3E, 0x42, 0xEA, 0x01, 0xA0}); // Startup marker after enabling.
        if (doubleSpeed) emit({0xF0, 0x4D, 0xEA, 0x02, 0xA0}); // Record the active clock rate.
        emit({0xF0, 0x56, 0xEA, 0x00, 0xA0, 0x18, 0xF9}); // Poll and record RP.
    }
    return rom;
}

template<class Predicate>
void runUntil(GameBoySystem &system, Predicate done, unsigned limit, const char *message) {
    for (unsigned cycle = 0; cycle < limit && !done(); ++cycle)
        expect(system.stepOneCycle().success, message);
    expect(done(), message);
}

void testConnectedCgbPortsAcrossClockRatesAndSnapshot() {
    GameBoySystem emitter(makeIrGuest(true, false), HardwareModel::CgbE);
    GameBoySystem normal(makeIrGuest(false, false), HardwareModel::CgbE);
    GameBoySystem fast(makeIrGuest(false, true), HardwareModel::CgbE);
    normal.setInfraredInputCallback([&] { return emitter.getInfraredLedState(); });
    fast.setInfraredInputCallback([&] { return emitter.getInfraredLedState(); });
    runUntil(emitter, [&] { return emitter.getInfraredLedState(); }, 100,
             "CGB guest turns on the IR LED through FF56");
    runUntil(normal, [&] { return normal.getSaveRamData()[1] == 0x42; }, 1000,
             "normal-speed CGB guest enables its IR receiver");
    runUntil(fast, [&] { return fast.getSaveRamData()[1] == 0x42; }, 4000,
             "double-speed CGB guest enables its IR receiver after KEY1 STOP");
    for (unsigned cycle = 0; cycle < 20; ++cycle)
        expect(normal.stepOneCycle().success && fast.stepOneCycle().success,
               "both guests poll FF56 shortly after enabling reception");
    expect(normal.getSaveRamData()[0] == 0xFE && fast.getSaveRamData()[0] == 0xFE,
           "both CGB clock rates hide incoming light during sensor settling");
    expect(fast.getSaveRamData()[2] == 0xFE,
           "the CPU probe confirms the receiver test is running in CGB double speed");

    for (unsigned cycle = 0; cycle < 100; ++cycle)
        expect(normal.stepOneCycle().success && fast.stepOneCycle().success,
               "both receivers continue while their pending warm-up is snapshotted");
    const auto pending = normal.saveState();
    GameBoySystem replay(makeIrGuest(false, false), HardwareModel::CgbE);
    replay.setInfraredInputCallback([&] { return emitter.getInfraredLedState(); });
    replay.loadState(pending.data(), pending.size());
    expect(pending[8] == 13 && replay.saveState() == pending,
           "current CGB snapshot preserves receiver settling and leaves host IR wiring external");

    constexpr unsigned FULL_SETTLE_M_CYCLES = CgbIo::IR_RECEIVER_SETTLE_T_CYCLES / 4;
    for (unsigned cycle = 0; cycle < 10000; ++cycle) {
        expect(normal.stepOneCycle().success && replay.stepOneCycle().success && fast.stepOneCycle().success,
               "normal and double-speed receivers remain in the pending settle interval");
    }
    expect(normal.getSaveRamData()[0] == 0xFE && replay.getSaveRamData()[0] == 0xFE &&
               normal.saveState() == replay.saveState(),
           "snapshot replay preserves the still-pending IR delay and peer callback");
    for (unsigned cycle = 0; cycle < FULL_SETTLE_M_CYCLES; ++cycle) {
        expect(normal.stepOneCycle().success && replay.stepOneCycle().success && fast.stepOneCycle().success,
               "normal and double-speed receivers advance through the base-clock settling period");
    }
    expect(normal.getSaveRamData()[0] == 0xFC && replay.getSaveRamData()[0] == 0xFC,
           "normal-speed peers see the emitter after 50 ms of base-clock time");
    expect(fast.getSaveRamData()[0] == 0xFE,
           "double-speed CPU receives only half the base-time settling clocks per M-cycle");
    const auto litState = normal.saveState();
    GameBoySystem litReplay(makeIrGuest(false, false), HardwareModel::CgbE);
    litReplay.setInfraredInputCallback([&] { return emitter.getInfraredLedState(); });
    litReplay.loadState(litState.data(), litState.size());
    expect(litReplay.saveState() == litState,
           "current CGB snapshot preserves a latched active sensor result and external peer wiring");
    for (unsigned cycle = 0; cycle < 20; ++cycle)
        expect(normal.stepOneCycle().success && replay.stepOneCycle().success && litReplay.stepOneCycle().success,
               "normal-speed receiver resumes from a latched-light snapshot");
    expect(normal.getSaveRamData()[0] == 0xFC && litReplay.getSaveRamData()[0] == 0xFC &&
               normal.saveState() == litReplay.saveState(),
           "restored active-low input remains visible to guest code");
    for (unsigned cycle = 0; cycle < FULL_SETTLE_M_CYCLES + 100; ++cycle)
        expect(fast.stepOneCycle().success, "double-speed receiver completes the physical-time delay");
    expect(fast.getSaveRamData()[0] == 0xFC,
           "double-speed peer detects the same IR signal after 50 ms of base time");
    expect(normal.takeAudioSamples() == replay.takeAudioSamples() && normal.saveState() == replay.saveState(),
           "snapshot replay preserves connected IR results and the complete guest state");
}
}

int main() {
    testRegisterAndReceiverWarmup();
    testConnectedCgbPortsAcrossClockRatesAndSnapshot();
    std::cout << "CGB infrared port, settling, clock-rate and snapshot tests passed\n";
}
