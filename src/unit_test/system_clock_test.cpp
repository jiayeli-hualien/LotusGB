#include <cstdlib>
#include <functional>
#include <iostream>
#include <vector>

#include "system_clock.hpp"

namespace {

using namespace LOTUSGB;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class ScriptedCpu final : public ICPU {
public:
    void reset() override {}
    void fetchFirstOpcode() override {}
    RetStepOneCycle stepOneCycle() override {
        if (nextResult >= results.size())
            return {};
        if (duringStep)
            duringStep();
        return results[nextResult++];
    }
    Reg getReg() override { return {}; }
    void setReg(const Reg &) override {}
    bool getIME() override { return false; }
    void setIME(bool) override {}
    bool getHALT() override { return false; }
    void setHALT(bool) override {}
    bool getStopped() override { return false; }
    void wakeFromStop() override {}

    std::vector<RetStepOneCycle> results;
    size_t nextResult = 0;
    std::function<void()> duringStep;
};

class ClockedSpy final : public IClockedComponent {
public:
    void advanceTCycles(uint64_t tCycles) override {
        advances.push_back(tCycles);
    }

    std::vector<uint64_t> advances;
};

void testSystemClockDistributesSuccessfulCpuTime() {
    ScriptedCpu cpu;
    cpu.results = {
        {true, false, T_CYCLES_PER_M_CYCLE},
        {true, false, 0},
        {true, true, T_CYCLES_PER_M_CYCLE},
        {false, false, 0},
    };
    ClockedSpy timer;
    ClockedSpy ppu;
    SystemClock clock(cpu);
    clock.addClockedComponent(timer);
    clock.addClockedComponent(ppu);

    expect(clock.step().success, "system clock accepts a successful CPU cycle");
    expect(clock.getElapsedTCycles() == T_CYCLES_PER_M_CYCLE,
           "system clock accumulates elapsed T-cycles");
    expect(timer.advances == std::vector<uint64_t>{T_CYCLES_PER_M_CYCLE},
           "timer receives the CPU cycle duration");
    expect(ppu.advances == timer.advances, "all components receive the same elapsed time");

    expect(clock.step().success, "zero-time CPU step succeeds");
    expect(clock.getElapsedTCycles() == T_CYCLES_PER_M_CYCLE,
           "zero-time step does not advance system time");
    expect(timer.advances.size() == 1, "zero-time step does not tick peripherals");

    expect(clock.step().fetchedNewInst, "system clock preserves CPU step results");
    expect(clock.getElapsedTCycles() == 2 * T_CYCLES_PER_M_CYCLE,
           "system clock accumulates later CPU cycles");

    expect(!clock.step().success, "system clock reports CPU step failure");
    expect(clock.getElapsedTCycles() == 2 * T_CYCLES_PER_M_CYCLE,
           "failed CPU step does not advance system time");
    expect(timer.advances.size() == 2, "failed CPU step does not tick peripherals");
}

void testSystemClockAdvancesDevicesToCpuBusPhaseBeforeTransaction() {
    ScriptedCpu cpu;
    cpu.results = {{true, false, T_CYCLES_PER_M_CYCLE}};
    ClockedSpy apu;
    SystemClock clock(cpu);
    clock.addClockedComponent(apu);
    cpu.duringStep = [&clock] { clock.advanceToCpuBusAccess(); };

    expect(clock.step().success, "system clock accepts a CPU bus transaction");
    expect(apu.advances == std::vector<uint64_t>{T_CYCLES_PER_M_CYCLE},
           "devices advance to the CPU bus transaction at the M-cycle boundary");
    expect(clock.getElapsedTCycles() == T_CYCLES_PER_M_CYCLE,
           "bus transaction timing preserves total elapsed time");
}

void testDoubleSpeedSeparatesCpuAndBaseClock() {
    ScriptedCpu cpu; cpu.results={{true,false,4}};
    SystemClock clock(cpu);
    ClockedSpy timer,ppu;
    clock.addClockedComponent(timer,true); clock.addClockedComponent(ppu);
    clock.setDoubleSpeed(true);
    cpu.duringStep=[&] { clock.advanceToCpuBusAccess(); };
    expect(clock.step().tCyclesElapsed==2 && clock.getElapsedTCycles()==2,
           "double-speed M-cycle consumes two base T-cycles");
    expect(timer.advances==std::vector<uint64_t>{4} && ppu.advances==std::vector<uint64_t>{2},
           "CPU timer and base PPU receive different rates at the same bus access");
    clock.stepIdle();
    expect(timer.advances==std::vector<uint64_t>({4,4}) && ppu.advances==std::vector<uint64_t>({2,2}),
           "DMA idle clocks both domains while CPU execution is stalled");
    clock.stepIdle(false);
    expect(timer.advances.size()==2 && ppu.advances.size()==3 && clock.getElapsedTCycles()==6,
           "speed-switch pause freezes CPU devices while base time continues");
}

}

int main() {
    testDoubleSpeedSeparatesCpuAndBaseClock();
    testSystemClockDistributesSuccessfulCpuTime();
    testSystemClockAdvancesDevicesToCpuBusPhaseBeforeTransaction();
    std::cout << "System clock tests passed\n";
}
