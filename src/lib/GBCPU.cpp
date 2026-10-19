#include "include/GBCPU.hpp"
#include "include/gb_reg.hpp"
#include "include/Iinstruction.hpp"
#include "include/instSubFunc.hpp"
#include "include/IDecoder.hpp"

namespace LOTUSGB {

GBCPU::GBCPU(IMemoryAccess &memory, IDecoder &decoder, IDecoder &decoderCB)
:memory(memory), decoder(decoder), decoderCB(decoderCB) {
}

IInstruction* GBCPU::fetch(uint16_t addr) {
    // TODO: handle deced failed
    const uint8_t OPCODE_CB = 0xCB;
    instStat.opcode = memory.read(addr);
    if (waitDecodeCB) {
        waitDecodeCB = false;
        return decoderCB.decode(instStat.opcode);
    }
    waitDecodeCB = (instStat.opcode == OPCODE_CB);
    return decoder.decode(instStat.opcode);
}

void GBCPU::fetchFirstOpcode() {
    waitDecodeCB = false;
    instStat = {};
    pInst = fetch(reg.getRefPC()++);
}

void GBCPU::reset() {
    reg = {};
    clockTimeStamp = 0;
    interruptMasterEnable = true;
    isHalt = false;
    isStopped = false;
    fetchAfterStopWake = false;
    imeEnablePending = false;
    waitDecodeCB = false;
    instStat = {};
    // load first op
    fetchFirstOpcode();
}

void GBCPU::doFetchNextOp() {
    instStat = {};
    pInst = fetch(reg.getRefPC()++);
}

void GBCPU::doMemRead(InstState &instStat) {
    instStat.memValue = memory.read(instStat.memAddr);
}
void GBCPU::doMemReadAddrLSB(InstState &instStat) {
    const uint8_t tmp = memory.read(instStat.memAddr);
    instStat.a16Addr = HIGH_MEM_ADDR | tmp;
}
void GBCPU::doMemReadAddrMSB(InstState &instStat) {
    const uint8_t tmp = memory.read(instStat.memAddr);
    instStat.a16Addr = (tmp << 8U) | (instStat.a16Addr & 0xFF);
}
void GBCPU::doMemWrite(InstState &instStat) {
    memory.write(instStat.memAddr, instStat.memValue);
}

bool GBCPU::stepInstruction() {
    if (!pInst) {
        std::cerr << "last decode failed" << std::endl;
        return false;
    }

    if (!pInst->stepOneMemCycle(&instStat, &reg)) {
        std::cerr << "instruction failed" << std::endl;
        return false;
    }
    return true;
}

RetStepOneCycle GBCPU::stepOneCycle() {
    RetStepOneCycle ret;
    if (isStopped) {
        // STOP freezes the CPU clock until the system explicitly wakes it.
        ret.success = true;
        return ret;
    }
    if (fetchAfterStopWake) {
        // Keep the resumed opcode bus read inside a CPU machine cycle.
        fetchAfterStopWake = false;
        doFetchNextOp();
        clockTimeStamp += T_CYCLES_PER_M_CYCLE;
        ret.fetchedNewInst = true;
        ret.success = true;
        return ret;
    }
    if (isHalt) {
        // The system clock continues while HALTed; an interrupt controller can
        // wake the CPU through setHALT(false).
        clockTimeStamp += T_CYCLES_PER_M_CYCLE;
        ret.success = true;
        return ret;
    }

    instStat.memMode = MEM_MODE_FETCH;
    instStat.imeAction = IME_ACTION_NONE;
    instStat.clockAction = CLOCK_ACTION_NONE;

    if (!stepInstruction())
        return ret;

    const bool enableImeAfterNextInstruction =
        instStat.imeAction == IME_ACTION_ENABLE_DELAYED;
    switch (instStat.imeAction) {
        case IME_ACTION_ENABLE:
            setIME(true);
            imeEnablePending = false;
            break;
        case IME_ACTION_ENABLE_DELAYED:
            break;
        case IME_ACTION_DISABLE:
            setIME(false);
            imeEnablePending = false;
            break;
        case IME_ACTION_NONE:
        default:
            break;
    }

    if (instStat.clockAction == CLOCK_ACTION_HALT)
        setHALT(true);
    else if (instStat.clockAction == CLOCK_ACTION_STOP)
        isStopped = true;

    ret.fetchedNewInst = (!waitDecodeCB)
                         && (instStat.memMode == MEM_MODE_FETCH);

    if (ret.fetchedNewInst && imeEnablePending) {
        setIME(true);
        imeEnablePending = false;
    }
    if (enableImeAfterNextInstruction)
        imeEnablePending = true;

    switch (instStat.memMode) {
        case MEM_MODE_FETCH: doFetchNextOp(); break;
        case MEM_MODE_READ: doMemRead(instStat); break;
        case MEM_MODE_WRITE: doMemWrite(instStat); break;
        case MEM_MODE_READ_ADDR_LSB: doMemReadAddrLSB(instStat); break;
        case MEM_MODE_READ_ADDR_MSB: doMemReadAddrMSB(instStat); break;
        case MEM_MODE_SLEEP:
        default:
            break;
    }
    clockTimeStamp += T_CYCLES_PER_M_CYCLE;
    ret.success = true;
    return ret;
}

Reg GBCPU::getReg() {
    return reg;
}

void GBCPU::setReg(const Reg &reg) {
    this->reg = reg;
}

bool GBCPU::getIME() {
    return this->interruptMasterEnable;
}

void GBCPU::setIME(bool ime) {
    this->interruptMasterEnable = ime;
}

bool GBCPU::getHALT() {
    return this->isHalt;
}

void GBCPU::setHALT(bool halt) {
    this->isHalt = halt;
}

bool GBCPU::getStopped() {
    return isStopped;
}

void GBCPU::wakeFromStop() {
    if (!isStopped)
        return;
    isStopped = false;
    fetchAfterStopWake = true;
}

}
