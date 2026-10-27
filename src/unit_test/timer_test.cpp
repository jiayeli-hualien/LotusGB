#include <cstdlib>
#include <iostream>

#include "interrupt_registers.hpp"
#include "timer.hpp"

namespace {

using namespace LOTUSGB;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void testDividerAndTimerClockSelection() {
    InterruptRegisters interrupts;
    Timer timer(interrupts);

    timer.advanceTCycles(255);
    expect(timer.read(0xFF04) == 0, "DIV high byte remains zero for 255 T-cycles");
    timer.advanceTCycles(1);
    expect(timer.read(0xFF04) == 1, "DIV increments after 256 T-cycles");

    timer.write(0xFF07, 0x05); // Enable, select divider bit 3: one tick per 16 T-cycles.
    timer.advanceTCycles(15);
    expect(timer.read(0xFF05) == 0, "selected timer input has not had a falling edge yet");
    timer.advanceTCycles(1);
    expect(timer.read(0xFF05) == 1, "TIMA increments on selected divider-bit falling edge");
    expect(timer.read(0xFF07) == 0xFD, "unused TAC bits read as one");
}

void testDmgBootRomDividerPhase() {
    InterruptRegisters interrupts;
    Timer timer(interrupts);
    timer.initializeDmgBootRomState();
    timer.advanceTCycles(251);
    expect(timer.read(0xFF04) == 0,
           "DMG boot divider starts four cycles into the first increment period");
    timer.advanceTCycles(1);
    expect(timer.read(0xFF04) == 1,
           "DMG boot divider reaches its first increment at 256 total cycles");
}

void testDmgFastStartupUsesCapturedDividerPhase() {
    InterruptRegisters interrupts;
    Timer timer(interrupts);
    timer.initializeDmgPostBootState();
    expect(timer.read(0xFF04) == 0xAB,
           "fast startup preserves the captured DIV high byte");
    timer.advanceTCycles(55);
    expect(timer.read(0xFF04) == 0xAB,
           "captured handoff phase delays DIV until the next full period");
    timer.advanceTCycles(1);
    expect(timer.read(0xFF04) == 0xAC,
           "captured handoff phase reaches the next DIV value at the expected cycle");
}

void testAllTacFrequencies() {
    constexpr unsigned tCyclesPerIncrement[4] = {1024, 16, 64, 256};

    for (uint8_t selection = 0; selection < 4; ++selection) {
        InterruptRegisters interrupts;
        Timer timer(interrupts);
        timer.write(0xFF07, static_cast<uint8_t>(0x04 | selection));
        timer.advanceTCycles(tCyclesPerIncrement[selection] - 1);
        expect(timer.read(0xFF05) == 0,
               "TIMA waits for the selected divider signal's falling edge");
        timer.advanceTCycles(1);
        expect(timer.read(0xFF05) == 1,
               "TAC selection produces the documented timer frequency");
    }
}

void testDividerWriteCanCauseTimerEdge() {
    InterruptRegisters interrupts;
    Timer timer(interrupts);

    timer.write(0xFF07, 0x05);
    timer.advanceTCycles(8); // Divider bit 3 is now high.
    timer.write(0xFF04, 0xA5);

    expect(timer.read(0xFF04) == 0, "writing DIV resets the complete divider counter");
    expect(timer.read(0xFF05) == 1, "resetting a high selected divider bit clocks TIMA");
}

void testDividerApuClockUsesFallingEdgesAndDividerWrites() {
    InterruptRegisters interrupts;
    Timer timer(interrupts);
    unsigned apuClockEdges = 0;
    timer.setDividerApuClockCallback([&apuClockEdges] { ++apuClockEdges; });

    timer.advanceTCycles(8191);
    expect(apuClockEdges == 0,
           "DIV-APU does not clock before divider bit 12 falls");
    timer.advanceTCycles(1);
    expect(apuClockEdges == 1,
           "DIV-APU clocks on the divider bit 12 falling edge");

    timer.advanceTCycles(4096); // Raise divider bit 12 again.
    timer.write(0xFF04, 0xFF);
    expect(apuClockEdges == 2,
           "writing DIV while divider bit 12 is high clocks DIV-APU");
    timer.write(0xFF04, 0x00);
    expect(apuClockEdges == 2,
           "writing DIV while divider bit 12 is low does not clock DIV-APU");

    timer.advanceTCycles(8192);
    expect(apuClockEdges == 3,
           "DIV-APU resumes at the natural divider falling edge after reset");
}

void testOverflowReloadAndInterruptAreDelayed() {
    InterruptRegisters interrupts;
    Timer timer(interrupts);

    timer.write(0xFF06, 0x42);
    timer.write(0xFF05, 0xFF);
    timer.write(0xFF07, 0x05);
    timer.advanceTCycles(16);
    expect(timer.read(0xFF05) == 0, "TIMA is zero immediately after overflow");
    expect(interrupts.read(0xFF0F) == 0xE0,
           "timer interrupt is not requested on the overflow edge");

    timer.advanceTCycles(3);
    expect(timer.read(0xFF05) == 0, "TIMA remains zero during overflow delay");
    expect(interrupts.read(0xFF0F) == 0xE0,
           "timer interrupt remains clear during overflow delay");
    timer.advanceTCycles(1);
    expect(timer.read(0xFF05) == 0x42, "TIMA reloads TMA after four T-cycles");
    expect((interrupts.read(0xFF0F) & 0x04) != 0,
           "timer interrupt flag is requested when TIMA reloads");
}

void testTimaWriteCancelsPendingReload() {
    InterruptRegisters interrupts;
    Timer timer(interrupts);

    timer.write(0xFF06, 0x42);
    timer.write(0xFF05, 0xFF);
    timer.write(0xFF07, 0x05);
    timer.advanceTCycles(16);
    timer.write(0xFF05, 0x99);
    timer.advanceTCycles(4);

    expect(timer.read(0xFF05) == 0x99, "TIMA write cancels the pending overflow reload");
    expect((interrupts.read(0xFF0F) & 0x04) == 0,
           "cancelled overflow does not request the timer interrupt");
}

// Exercise observable writes on both sides of the four-T-cycle reload window.
void testWritesAcrossOverflowAndReloadWindow() {
    for (unsigned offset = 0; offset <= 8; ++offset) {
        InterruptRegisters interrupts;
        Timer timer(interrupts);
        timer.write(0xFF06, 0x42);
        timer.write(0xFF05, 0xFF);
        timer.write(0xFF07, 0x05);
        timer.advanceTCycles(16 + offset);
        timer.write(0xFF05, 0x99);
        if (offset < 4) {
            timer.advanceTCycles(8 - offset);
            expect(timer.read(0xFF05) == 0x99,
                   "TIMA writes before reload cancel the entire pending reload");
            expect((interrupts.read(0xFF0F) & 4) == 0,
                   "cancelled reload does not request an interrupt");
        } else {
            expect(timer.read(0xFF05) == (offset < 8 ? 0x42 : 0x99),
                   "TIMA writes are ignored throughout reload, then accepted");
            expect((interrupts.read(0xFF0F) & 4) != 0,
                   "writes during or after reload do not cancel its interrupt");
        }
    }

    for (unsigned offset = 0; offset <= 8; ++offset) {
        InterruptRegisters interrupts;
        Timer timer(interrupts);
        timer.write(0xFF06, 0x42);
        timer.write(0xFF05, 0xFF);
        timer.write(0xFF07, 0x05);
        timer.advanceTCycles(16 + offset);
        timer.write(0xFF06, 0x77);
        expect(timer.read(0xFF06) == 0x77, "TMA writes always update TMA");
        expect(timer.read(0xFF05) == (offset < 4 ? 0 : offset < 8 ? 0x77 : 0x42),
               "TMA writes update TIMA only during the reload window");
        if (offset < 4) {
            timer.advanceTCycles(4 - offset);
            expect(timer.read(0xFF05) == 0x77,
                   "pending reload picks up the latest TMA value");
            expect((interrupts.read(0xFF0F) & 4) != 0,
                   "TMA write does not cancel the pending interrupt");
        }
    }
}

void testInterruptRegistersMaskAndExposeRequests() {
    InterruptRegisters interrupts;
    interrupts.write(0xFFFF, 0x05);
    interrupts.write(0xFF0F, 0xE0);
    expect(interrupts.read(0xFFFF) == 0x05, "IE stores enabled interrupt bits");
    expect(interrupts.read(0xFF0F) == 0xE0, "unused IF bits read as one");
    expect(interrupts.getRequestedInterrupts() == 0, "no interrupt is pending initially");

    interrupts.requestTimerInterrupt();
    expect(interrupts.read(0xFF0F) == 0xE4, "timer request sets IF bit 2");
    expect(interrupts.getRequestedInterrupts() == 0x04,
           "pending requests are masked by IE");
}

}

int main() {
    testDividerAndTimerClockSelection();
    testDmgBootRomDividerPhase();
    testDmgFastStartupUsesCapturedDividerPhase();
    testAllTacFrequencies();
    testDividerWriteCanCauseTimerEdge();
    testDividerApuClockUsesFallingEdgesAndDividerWrites();
    testOverflowReloadAndInterruptAreDelayed();
    testTimaWriteCancelsPendingReload();
    testWritesAcrossOverflowAndReloadWindow();
    testInterruptRegistersMaskAndExposeRequests();
    std::cout << "Timer tests passed\n";
}
