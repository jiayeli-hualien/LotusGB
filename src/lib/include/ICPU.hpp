#ifndef LOTUSGB_ICPU_HPP
#define LOTUSGB_ICPU_HPP

#include<iostream>
#include <iostream>
#include "gb_reg.hpp"

namespace LOTUSGB {

// The current CPU stepping interface advances one machine cycle at a time.
// Each M-cycle contains four CPU T-cycles.
static constexpr unsigned T_CYCLES_PER_M_CYCLE = 4;

struct RetStepOneCycle {
    bool success = false;
    bool fetchedNewInst = false;
    unsigned tCyclesElapsed = 0;
};

class ICPU {
public:
    virtual ~ICPU() = default;
    virtual void reset() = 0;
    virtual void fetchFirstOpcode() = 0;

    // Advance the CPU by one machine cycle (T_CYCLES_PER_M_CYCLE T-cycles).
    virtual RetStepOneCycle stepOneCycle() = 0;

    // tool functions for GBIT
    // TODO: remove them from API, or provide Memento Pattern
    virtual Reg getReg() = 0;
    virtual void setReg(const Reg &reg) = 0;
    virtual bool getIME() = 0;
    virtual void setIME(bool ime) = 0;
    virtual bool getHALT() = 0;
    virtual void setHALT(bool halt) = 0;
    virtual bool getStopped() = 0;
    virtual void wakeFromStop() = 0;
};

}

#endif // LOTUSGB_ICPU_HPP
