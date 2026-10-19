#include "include/system_clock.hpp"

namespace LOTUSGB {

SystemClock::SystemClock(ICPU &cpu) : cpu(cpu) {}

void SystemClock::addClockedComponent(IClockedComponent &component) {
    components.push_back(&component);
}

RetStepOneCycle SystemClock::step() {
    RetStepOneCycle result = cpu.stepOneCycle();
    if (!result.success)
        return result;

    elapsedTCycles += result.tCyclesElapsed;
    if (result.tCyclesElapsed != 0) {
        for (IClockedComponent *component : components)
            component->advanceTCycles(result.tCyclesElapsed);
    }
    return result;
}

uint64_t SystemClock::getElapsedTCycles() const {
    return elapsedTCycles;
}

}
