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

// The system-level time authority. Clocked components advance up to a CPU bus
// access before the memory transaction, then through the remainder of the M-cycle.
class SystemClock {
public:
    explicit SystemClock(ICPU &cpu);

    // CPU and components are borrowed; they must outlive this clock.
    void addClockedComponent(IClockedComponent &component);
    // Called by the CPU bus immediately before a memory transaction.
    void advanceToCpuBusAccess();
    RetStepOneCycle step();
    uint64_t getElapsedTCycles() const;

private:
    ICPU &cpu;
    std::vector<IClockedComponent *> components;
    uint64_t elapsedTCycles = 0;
    uint64_t currentStepTCyclesAdvanced = 0;
    bool cpuStepInProgress = false;

    void advanceComponents(uint64_t tCycles);
};

}

#endif // LOTUSGB_SYSTEM_CLOCK_HPP
