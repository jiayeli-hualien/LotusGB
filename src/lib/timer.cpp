#include "include/timer.hpp"

#include <utility>

namespace LOTUSGB {

namespace {
constexpr uint16_t DIV_ADDRESS = 0xFF04;
constexpr uint16_t TIMA_ADDRESS = 0xFF05;
constexpr uint16_t TMA_ADDRESS = 0xFF06;
constexpr uint16_t TAC_ADDRESS = 0xFF07;

constexpr uint8_t timerCounterBitForTacValue[4] = {9, 3, 5, 7};
constexpr uint8_t DIV_APU_COUNTER_BIT = 12;
constexpr int OVERFLOW_RELOAD_DELAY_T_CYCLES = 4;
}

Timer::Timer(IInterruptController &interrupts) : interrupts(interrupts) {}

void Timer::setDividerApuClockCallback(std::function<void()> callback) {
    dividerApuClockCallback = std::move(callback);
}

void Timer::initializeDmgPostBootState() {
    systemCounter = 0xAB00;
    tima = 0;
    tma = 0;
    tac = 0;
    overflowReloadTCycles = 0;
    reloadWindowTCycles = 0;
}

uint8_t Timer::read(uint16_t address) {
    switch (address) {
        case DIV_ADDRESS:
            return static_cast<uint8_t>(systemCounter >> 8);
        case TIMA_ADDRESS:
            return tima;
        case TMA_ADDRESS:
            return tma;
        case TAC_ADDRESS:
            return static_cast<uint8_t>(0xF8 | tac);
        default:
            return 0xFF;
    }
}

void Timer::write(uint16_t address, uint8_t value) {
    switch (address) {
        case DIV_ADDRESS: {
            const bool oldInput = timerInput();
            const bool oldDividerApuInput = dividerApuInput();
            systemCounter = 0;
            applyTimerInputTransition(oldInput);
            applyDividerApuTransition(oldDividerApuInput);
            break;
        }
        case TIMA_ADDRESS:
            // Writes in the delay cancel reload; the following reload M-cycle
            // ignores TIMA writes for all four T-cycles.
            if (reloadWindowTCycles > 0)
                break;
            tima = value;
            if (overflowReloadTCycles > 0)
                overflowReloadTCycles = 0;
            break;
        case TMA_ADDRESS:
            tma = value;
            if (reloadWindowTCycles > 0)
                tima = value;
            break;
        case TAC_ADDRESS: {
            const bool oldInput = timerInput();
            tac = value & 0x07;
            applyTimerInputTransition(oldInput);
            break;
        }
        default:
            break;
    }
}

void Timer::advanceTCycles(uint64_t tCycles) {
    for (uint64_t cycle = 0; cycle < tCycles; ++cycle) {
        if (reloadWindowTCycles > 0)
            --reloadWindowTCycles;
        if (overflowReloadTCycles > 0) {
            --overflowReloadTCycles;
            if (overflowReloadTCycles == 0) {
                // The load signal remains active for this entire M-cycle:
                // TIMA writes are ignored and TMA writes also update TIMA.
                reloadWindowTCycles = 4;
                tima = tma;
                interrupts.requestInterrupt(INTERRUPT_TIMER);
            }
        }

        const bool oldInput = timerInput();
        const bool oldDividerApuInput = dividerApuInput();
        ++systemCounter;
        applyTimerInputTransition(oldInput);
        applyDividerApuTransition(oldDividerApuInput);
    }
}

bool Timer::timerInput() const {
    if ((tac & 0x04) == 0)
        return false;

    const uint8_t selectedBit = timerCounterBitForTacValue[tac & 0x03];
    return (systemCounter & (1U << selectedBit)) != 0;
}

bool Timer::dividerApuInput() const {
    return (systemCounter & (1U << DIV_APU_COUNTER_BIT)) != 0;
}

void Timer::incrementTima() {
    if (overflowReloadTCycles != 0)
        return;

    if (tima == 0xFF) {
        tima = 0;
        overflowReloadTCycles = OVERFLOW_RELOAD_DELAY_T_CYCLES;
    } else {
        ++tima;
    }
}

void Timer::applyTimerInputTransition(bool oldInput) {
    if (oldInput && !timerInput())
        incrementTima();
}

void Timer::applyDividerApuTransition(bool oldInput) {
    if (oldInput && !dividerApuInput() && dividerApuClockCallback)
        dividerApuClockCallback();
}

}
