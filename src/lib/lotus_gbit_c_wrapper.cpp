#include <memory>
#include "include/lotus_gbit_c_wrapper.h"
#include "include/ILotusGBIT.hpp"
#include "include/lotusGBIT.hpp"
#include "include/GBCPU.hpp"
#include "include/memory.hpp"
#include "include/memoryGBIT.hpp"
#include "include/Iinstruction.hpp"
#include "include/IDecoder.hpp"
#include "include/decoder.hpp"

using LOTUSGB::LotusGBIT;
using LOTUSGB::GBCPU;
using LOTUSGB::Memory;
using LOTUSGB::MemoryGBIT;
using LOTUSGB::IDecoder;
using LOTUSGB::Decoder;
using std::make_shared;
using std::make_unique;

// TODO: IDecoder & pass shared_ptr into LotusGBIT
static IDecoder *pDecoder = nullptr;
static IDecoder *pDecoderCB = nullptr;
pILotusGBIT lotusGBIT_Create() {
    // TODO: factory
    // TODO: factory for decoder
    // TODO: delete objects
    if (!pDecoder)
        pDecoder = new Decoder(false, true);
    if (!pDecoderCB)
        pDecoderCB = new Decoder(true);
    auto spMockMMU = make_shared<MemoryGBIT>();
    auto upCPU = make_unique<GBCPU>(spMockMMU.get(), pDecoder, pDecoderCB);
    return new LotusGBIT(std::move(upCPU), spMockMMU);
}

void lotusGBIT_Destroy(pILotusGBIT pTestObj) {
    delete pDecoder;
    pDecoder = nullptr;
    delete pDecoderCB;
    pDecoder = nullptr;
    delete pTestObj;
}

void lotusGBIT_Init(pILotusGBIT self, size_t instruction_mem_size, uint8_t *instruction_mem) {
    self->init(instruction_mem_size, instruction_mem);
}

void lotusGBIT_GetStatus(pILotusGBIT self, LotusGBState *pState) {
    self->getState(pState);
}

void lotusGBIT_SetStatus(pILotusGBIT self, LotusGBState *pState) {
    self->setState(pState);
}

int lotusGBIT_Step(pILotusGBIT self) {
    return self->step();
}
