#ifndef LOTUSGB_SYSTEM_CLOCK_HPP
#define LOTUSGB_SYSTEM_CLOCK_HPP

#include <cstdint>
#include <vector>

#include "ICPU.hpp"

namespace LOTUSGB {

class IClockedComponent {
public:
    virtual ~IClockedComponent() = default;
    virtual void advanceTCycles(uint64_t tCycles) = 0;
};

// The system-level time authority. Clocked components advance through the
// modeled M-cycle before the CPU performs its bus transaction.
class SystemClock {
    friend class StateCodec;
public:
    explicit SystemClock(ICPU &cpu);

    // CPU and components are borrowed; they must outlive this clock.
    void addClockedComponent(IClockedComponent &component, bool cpuRate = false);
    void setDoubleSpeed(bool enabled) { doubleSpeed = enabled; }
    RetStepOneCycle stepIdle(bool clockCpuDevices = true);
    // Called by the CPU bus immediately before a memory transaction. The
    // current model advances devices to the end of the M-cycle here.
    void advanceToCpuBusAccess();
    RetStepOneCycle step();
    uint64_t getElapsedTCycles() const;
    unsigned baseClocksPerMCycle() const { return doubleSpeed ? 2 : 4; }

private:
    ICPU &cpu;
    struct ClockedDevice { IClockedComponent *component; bool cpuRate; };
    std::vector<ClockedDevice> components;
    bool doubleSpeed = false;
    uint64_t elapsedTCycles = 0;
    uint64_t currentStepTCyclesAdvanced = 0;
    bool cpuStepInProgress = false;

    void advanceComponents(uint64_t tCycles, bool clockCpuDevices = true);
};

}

#endif // LOTUSGB_SYSTEM_CLOCK_HPP
