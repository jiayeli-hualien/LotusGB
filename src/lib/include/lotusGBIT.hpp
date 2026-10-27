/*
Lotus GB detail implementation.
implement GB related interfaces to emu a GB.
*/
#ifndef LOTUS_GB_HPP
#define LOTUS_GB_HPP
#include<memory>
#include "ICPU.hpp"
#include "IDecoder.hpp"
#include "IGBITCPU.hpp"
#include "ILotusGBIT.hpp"
#include "IMemoryGBIT.hpp"
#include "gb_reg.hpp"

namespace LOTUSGB {

using std::shared_ptr;
using std::unique_ptr;

class LotusGBIT : public ILotusGBIT {
public:
    LotusGBIT(unique_ptr<ICPU> &&upCPU, shared_ptr<IMemoryGBIT> spMMU,
              shared_ptr<IDecoder> spDecoder, shared_ptr<IDecoder> spDecoderCB);
    ~LotusGBIT() override = default;
    void init(size_t instruction_mem_size, uint8_t *instruction_mem) override;
    void getState(LotusGBState *pState) override;
    void setState(LotusGBState *pState) override;
    int step() override; // step one CPU instruction
private:
    // Keep borrowed CPU dependencies alive; members are destroyed after upCPU.
    shared_ptr<IMemoryGBIT> spMMU;
    shared_ptr<IDecoder> spDecoder;
    shared_ptr<IDecoder> spDecoderCB;
    unique_ptr<ICPU> upCPU;
    unique_ptr<IGBITCPU> upGBITCPU;
};

}


#endif // LOTUS_GB_HPP
