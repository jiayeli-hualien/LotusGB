#include "include/system_clock.hpp"

namespace LOTUSGB {

SystemClock::SystemClock(ICPU &cpu) : cpu(cpu) {}

void SystemClock::addClockedComponent(IClockedComponent &component, bool cpuRate) {
    components.push_back({&component, cpuRate});
}

void SystemClock::advanceComponents(uint64_t tCycles, bool clockCpuDevices) {
    if (tCycles == 0)
        return;
    for (const auto &device : components)
        if (!device.cpuRate || clockCpuDevices)
            device.component->advanceTCycles(device.cpuRate ? tCycles : tCycles / (doubleSpeed ? 2 : 1));
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

    const auto elapsed = result.tCyclesElapsed / (doubleSpeed ? 2 : 1);
    elapsedTCycles += elapsed;
    if (result.tCyclesElapsed > currentStepTCyclesAdvanced)
        advanceComponents(result.tCyclesElapsed - currentStepTCyclesAdvanced);
    result.tCyclesElapsed = elapsed;
    return result;
}

RetStepOneCycle SystemClock::stepIdle(bool clockCpuDevices) {
    advanceComponents(4, clockCpuDevices);
    const uint64_t elapsed = doubleSpeed ? 2 : 4;
    elapsedTCycles += elapsed;
    RetStepOneCycle result;
    result.success = true;
    result.tCyclesElapsed = elapsed;
    return result;
}

uint64_t SystemClock::getElapsedTCycles() const {
    return elapsedTCycles;
}

}
