#include "include/system_clock.hpp"

namespace LOTUSGB {

SystemClock::SystemClock(ICPU &cpu) : cpu(cpu) {}

void SystemClock::addClockedComponent(IClockedComponent &component) {
    components.push_back(&component);
}

void SystemClock::advanceComponents(uint64_t tCycles) {
    if (tCycles == 0)
        return;
    for (IClockedComponent *component : components)
        component->advanceTCycles(tCycles);
}

void SystemClock::advanceToCpuBusAccess() {
    if (!cpuStepInProgress || currentStepTCyclesAdvanced != 0)
        return;

    constexpr uint64_t CPU_BUS_PHASE_T_CYCLES = T_CYCLES_PER_M_CYCLE;
    advanceComponents(CPU_BUS_PHASE_T_CYCLES);
    currentStepTCyclesAdvanced = CPU_BUS_PHASE_T_CYCLES;
}

RetStepOneCycle SystemClock::step() {
    currentStepTCyclesAdvanced = 0;
    cpuStepInProgress = true;
    RetStepOneCycle result = cpu.stepOneCycle();
    cpuStepInProgress = false;
    if (!result.success)
        return result;

    elapsedTCycles += result.tCyclesElapsed;
    if (result.tCyclesElapsed > currentStepTCyclesAdvanced)
        advanceComponents(result.tCyclesElapsed - currentStepTCyclesAdvanced);
    return result;
}

uint64_t SystemClock::getElapsedTCycles() const {
    return elapsedTCycles;
}

}
