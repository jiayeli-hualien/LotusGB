#ifndef LOTUSGB_GBCPU_HPP
#define LOTUSGB_GBCPU_HPP

#include "ICPU.hpp"
#include "IMemoryAccess.hpp"
#include "gb_reg.hpp"
#include "IDecoder.hpp"

namespace LOTUSGB {

// TODO: refactor the naming: GBCPU->GbCpu, find a tool
class GBCPU : public ICPU {
public:
    GBCPU(IMemoryAccess &memory, IDecoder &decoder, IDecoder &decoderCB);
    void reset();
    RetStepOneCycle stepOneCycle();

    void fetchFirstOpcode();
    Reg getReg();
    void setReg(const Reg &reg);
    bool getIME();
    void setIME(bool ime);
    bool getHALT();
    void setHALT(bool halt);
    bool getStopped();
    void wakeFromStop();
private:
    IInstruction* fetch(uint16_t addr);
    void doFetchNextOp();
    void doMemRead(InstState &instStat);
    void doMemReadAddrMSB(InstState &instStat);
    void doMemReadAddrLSB(InstState &instStat);
    void doMemWrite(InstState &instStat);
    bool stepInstruction();

    // These dependencies are borrowed; their owner must outlive the CPU.
    IMemoryAccess &memory;
    IDecoder &decoder;
    IDecoder &decoderCB;
    Reg reg;
    u_int64_t clockTimeStamp;
    bool interruptMasterEnable = true;
    bool isHalt = false;
    bool isStopped = false;
    bool fetchAfterStopWake = false;
    bool imeEnablePending = false;
    bool waitDecodeCB = false;
    InstState instStat;
    IInstruction *pInst;
};

}

#endif // LOTUSGB_CPU_HPP
