#include <iostream>
#include <cstring>
#include "include/lotusGBIT.hpp"
#include "include/GBITCPU.hpp"

namespace LOTUSGB {

LotusGBIT::LotusGBIT(unique_ptr<ICPU> &&upCPU, shared_ptr<IMemoryGBIT> spMMU,
                     shared_ptr<IDecoder> spDecoder,
                     shared_ptr<IDecoder> spDecoderCB)
:spMMU(std::move(spMMU)), spDecoder(std::move(spDecoder)),
 spDecoderCB(std::move(spDecoderCB)), upCPU(std::move(upCPU)) {
    // TODO: only access via interfaces
    upGBITCPU = std::make_unique<GBITCPU>(this->upCPU.get());
}

void LotusGBIT::getState(LotusGBState *pState) {
    // TODO: copy all status
    memset(pState, 0, sizeof(LotusGBState));
    Reg reg = upCPU->getReg();
    // gbit reference CPU don't have parallel fetch/excute overlap
    // While stopped, the CPU has consumed STOP's padding byte and has not
    // prefetched the next opcode, so PC already points at the next instruction.
    pState->pc = upCPU->getStopped() ? reg.getRefPC() : reg.getRefPC() - 1;
    pState->sp = reg.getRefSP();
    pState->a = reg.getRefA();
    pState->f = reg.getRefF();
    pState->b = reg.getRefB();
    pState->c = reg.getRefC();
    pState->d = reg.getRefD();
    pState->e = reg.getRefE();
    pState->h = reg.getRefH();
    pState->l = reg.getRefL();
    pState->is_halt = upCPU->getHALT() || upCPU->getStopped();
    pState->interrupts_master_enable = upCPU->getIME();
    pState->num_accesses = spMMU->getMemoryState(pState->mem_accesses);
}

void LotusGBIT::init(size_t instruction_mem_size, uint8_t *instruction_mem) {
    spMMU->setInsturctionMem(instruction_mem_size, instruction_mem);
}

void LotusGBIT::setState(LotusGBState *pState) {
    // TODO: copy all status
    Reg reg;
    upCPU->reset();
    reg.getRefPC() = pState->pc;
    reg.getRefSP() = pState->sp;
    reg.getRefA() = pState->a;
    reg.getRefF() = pState->f;
    reg.getRefB() = pState->b;
    reg.getRefC() = pState->c;
    reg.getRefD() = pState->d;
    reg.getRefE() = pState->e;
    reg.getRefH() = pState->h;
    reg.getRefL() = pState->l;
    upCPU->setReg(reg);
    upCPU->setHALT(pState->is_halt);
    upCPU->setIME(pState->interrupts_master_enable);

    spMMU->setMemoryState(pState->num_accesses, pState->mem_accesses);

    // workaround for pCPU->reset / setReg order
    upCPU->fetchFirstOpcode();
}

int LotusGBIT::step() {
   return upGBITCPU->stepOneInstruction()*4;
}

}
