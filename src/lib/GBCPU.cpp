#include "include/GBCPU.hpp"
#include "include/gb_reg.hpp"
#include "include/Iinstruction.hpp"
#include "include/instSubFunc.hpp"
#include "include/IDecoder.hpp"

namespace LOTUSGB {

GBCPU::GBCPU(IMemoryAccess &memory, IDecoder &decoder, IDecoder &decoderCB,
             IInterruptController *interruptController)
:memory(memory), decoder(decoder), decoderCB(decoderCB),
 interruptController(interruptController) {
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
    initialFetchPending = false;
    waitDecodeCB = false;
    instStat = {};
    pInst = fetch(reg.getRefPC()++);
    instructionBoundary = true;
}

void GBCPU::reset() {
    reg = {};
    interruptMasterEnable = true;
    isHalt = false;
    isStopped = false;
    fetchAfterStopWake = false;
    initialFetchPending = true;
    imeEnablePending = false;
    waitDecodeCB = false;
    instructionBoundary = false;
    haltBugPending = false;
    interruptSequenceActive = false;
    interruptSequenceCycle = 0;
    activeInterruptMask = 0;
    activeInterruptVector = 0;
    instStat = {};
    pInst = nullptr;
}

void GBCPU::doFetchNextOp() {
    instStat = {};
    uint16_t &pc = reg.getRefPC();
    const uint16_t fetchAddress = pc;
    const bool fetchingCbOpcode = waitDecodeCB;
    if (haltBugPending)
        haltBugPending = false;
    else
        ++pc;
    pInst = fetch(fetchAddress);
    // CB's second byte is part of the same instruction as the prefix. Do not
    // sample interrupts between fetching the CB opcode and executing it.
    instructionBoundary = !fetchingCbOpcode;
}

RetStepOneCycle GBCPU::stepInterruptCycle() {
    RetStepOneCycle ret;
    ret.success = true;
    ret.tCyclesElapsed = T_CYCLES_PER_M_CYCLE;

    switch (interruptSequenceCycle) {
        case 0:
        case 1:
            break;
        case 2: {
            uint16_t &sp = reg.getRefSP();
            --sp;
            memory.write(sp, static_cast<uint8_t>(reg.getRefPC() >> 8));
            break;
        }
        case 3: {
            uint16_t &sp = reg.getRefSP();
            --sp;
            memory.write(sp, static_cast<uint8_t>(reg.getRefPC() & 0xFF));
            break;
        }
        case 4:
            reg.getRefPC() = activeInterruptVector;
            doFetchNextOp();
            ret.fetchedNewInst = true;
            interruptSequenceActive = false;
            break;
        default:
            ret.success = false;
            return ret;
    }

    if (interruptSequenceActive)
        ++interruptSequenceCycle;
    return ret;
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
    if (initialFetchPending) {
        initialFetchPending = false;
        doFetchNextOp();
        ret.fetchedNewInst = true;
        ret.success = true;
        ret.tCyclesElapsed = T_CYCLES_PER_M_CYCLE;
        return ret;
    }
    if (fetchAfterStopWake) {
        // Keep the resumed opcode bus read inside a CPU machine cycle.
        fetchAfterStopWake = false;
        doFetchNextOp();
        ret.fetchedNewInst = true;
        ret.success = true;
        ret.tCyclesElapsed = T_CYCLES_PER_M_CYCLE;
        return ret;
    }

    if (interruptSequenceActive)
        return stepInterruptCycle();

    const uint8_t pendingInterrupts = interruptController
        ? interruptController->getPendingInterrupts() : 0;
    if (isHalt) {
        if (pendingInterrupts == 0) {
            ret.success = true;
            ret.tCyclesElapsed = T_CYCLES_PER_M_CYCLE;
            return ret;
        }
        isHalt = false;
    }

    if (instructionBoundary && interruptMasterEnable && pendingInterrupts != 0) {
        constexpr uint8_t interruptMasks[] = {
            INTERRUPT_VBLANK, INTERRUPT_LCD_STAT, INTERRUPT_TIMER,
            INTERRUPT_SERIAL, INTERRUPT_JOYPAD};
        constexpr uint16_t interruptVectors[] = {0x0040, 0x0048, 0x0050, 0x0058, 0x0060};
        for (size_t i = 0; i < 5; ++i) {
            if ((pendingInterrupts & interruptMasks[i]) == 0)
                continue;
            activeInterruptMask = interruptMasks[i];
            activeInterruptVector = interruptVectors[i];
            break;
        }

        // The opcode at PC-1 was prefetched at the preceding instruction's
        // boundary. Interrupt entry discards it and returns to its address.
        --reg.getRefPC();
        pInst = nullptr;
        waitDecodeCB = false;
        instructionBoundary = false;
        interruptMasterEnable = false;
        imeEnablePending = false;
        interruptController->acknowledgeInterrupt(activeInterruptMask);
        interruptSequenceActive = true;
        interruptSequenceCycle = 0;
        return stepInterruptCycle();
    }

    instructionBoundary = false;

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

    if (instStat.clockAction == CLOCK_ACTION_HALT) {
        if (!interruptMasterEnable && pendingInterrupts != 0) {
            isHalt = false;
            haltBugPending = true;
        } else {
            setHALT(true);
        }
    }
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
    ret.success = true;
    ret.tCyclesElapsed = T_CYCLES_PER_M_CYCLE;
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
