#ifndef LOTUSGB_DECODER_HPP
#define LOTUSGB_DECODER_HPP
#include "IDecoder.hpp"
#include "Iinstruction.hpp"
#include "instruction.hpp"


namespace LOTUSGB {

class Decoder : public IDecoder {
    // Instruct factory, get instruction object from opcode
    // TODO: refactor by abstraction factory pattern
public:
    explicit Decoder(bool cbOpcodeTable);
    IInstruction* decode(const uint8_t &opcode);
private:
    std::vector<Instruction> instList;
    uint8_t lutInstIdx[256];
};

}

#endif // #ifndef LOTUSGB_DECODER_HPP
