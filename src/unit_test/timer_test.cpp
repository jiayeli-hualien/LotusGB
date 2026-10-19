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
    testAllTacFrequencies();
    testDividerWriteCanCauseTimerEdge();
    testOverflowReloadAndInterruptAreDelayed();
    testTimaWriteCancelsPendingReload();
    testInterruptRegistersMaskAndExposeRequests();
    std::cout << "Timer tests passed\n";
}
