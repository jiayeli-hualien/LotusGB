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

// The system-level time authority. CPU bus operations still happen inside the
// CPU step; this class distributes the elapsed time to system components.
class SystemClock {
public:
    explicit SystemClock(ICPU &cpu);

    // CPU and components are borrowed; they must outlive this clock.
    void addClockedComponent(IClockedComponent &component);
    RetStepOneCycle step();
    uint64_t getElapsedTCycles() const;

private:
    ICPU &cpu;
    std::vector<IClockedComponent *> components;
    uint64_t elapsedTCycles = 0;
};

}

#endif // LOTUSGB_SYSTEM_CLOCK_HPP
