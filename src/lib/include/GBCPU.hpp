#ifndef LOTUSGB_GBCPU_HPP
#define LOTUSGB_GBCPU_HPP

#include "ICPU.hpp"
#include "IMemoryAccess.hpp"
#include "gb_reg.hpp"
#include "IDecoder.hpp"
#include "IInterruptController.hpp"

namespace LOTUSGB {

// TODO: refactor the naming: GBCPU->GbCpu, find a tool
class GBCPU : public ICPU {
    friend class StateCodec;
public:
    GBCPU(IMemoryAccess &memory, IDecoder &decoder, IDecoder &decoderCB,
          IInterruptController *interruptController = nullptr);
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
    IInstruction* fetch(uint16_t addr, bool increment = true);
    void doFetchNextOp();
    RetStepOneCycle stepInterruptCycle();
    void doMemRead(InstState &instStat);
    void doMemReadAddrMSB(InstState &instStat);
    void doMemReadAddrLSB(InstState &instStat);
    void doMemWrite(InstState &instStat);
    bool stepInstruction();

    // These dependencies are borrowed; their owner must outlive the CPU.
    IMemoryAccess &memory;
    IDecoder &decoder;
    IDecoder &decoderCB;
    IInterruptController *interruptController;
    Reg reg;
    bool interruptMasterEnable = true;
    bool isHalt = false;
    bool isStopped = false;
    bool isLocked = false;
    bool fetchAfterStopWake = false;
    bool initialFetchPending = false;
    bool imeEnablePending = false;
    bool waitDecodeCB = false;
    bool instructionBoundary = false;
    bool haltBugPending = false;
    bool interruptSequenceActive = false;
    uint8_t interruptSequenceCycle = 0;
    uint8_t activeInterruptMask = 0;
    uint16_t activeInterruptVector = 0;
    InstState instStat;
    IInstruction *pInst = nullptr;
};

}

#endif // LOTUSGB_CPU_HPP
