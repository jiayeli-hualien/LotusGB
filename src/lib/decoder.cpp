#include "include/decoder.hpp"
#include "include/generated_decode_tables.hpp"
#include <iostream>
#include <unordered_map>
#include <string>
#include <vector>

using std::string;
using std::vector;

namespace LOTUSGB {

template <typename DecodeRows, typename InstructionRows>
static void initializeTables(
    std::vector<Instruction> &instList, uint8_t (&lutInstIdx)[256],
    const DecodeRows &rules, const InstructionRows &instructionRows) {
    std::unordered_map<string, size_t> instIdx;
    for (const auto &row : instructionRows) {
        vector<IInstruction::FUNC_TYPE> functions;
        functions.reserve(row.cycles);
        for (size_t i = 0; i < row.cycles; ++i) {
            functions.emplace_back(row.functions[i]);
        }
        instIdx.emplace(string(row.name), instList.size());
        instList.emplace_back(string(row.name), row.cycles, std::move(functions));
    }

    for (int opcode = 0; opcode < 256; ++opcode) {
        lutInstIdx[opcode] = static_cast<uint8_t>(instList.size());
        for (const auto &rule : rules) {
            if ((opcode & rule.mask) == (rule.value & rule.mask)) {
                const auto it = instIdx.find(string(rule.instruction));
                if (it != instIdx.end())
                    lutInstIdx[opcode] = static_cast<uint8_t>(it->second);
                break;
            }
        }
    }
}

Decoder::Decoder(bool cbOpcodeTable) {
    if (cbOpcodeTable)
        initializeTables(instList, lutInstIdx, GeneratedDecodeTables::cbDecodeRules,
                         GeneratedDecodeTables::cbInstructions);
    else
        initializeTables(instList, lutInstIdx, GeneratedDecodeTables::decodeRules,
                         GeneratedDecodeTables::instructions);
}

IInstruction* Decoder::decode(const uint8_t &opcode) {
    if (lutInstIdx[opcode]>=instList.size()) {
        // TODO: verbose level
        return nullptr;
    }
    auto ret = &(instList[lutInstIdx[opcode]]);
    if (!ret) {
        std::cerr << "faild to get instruction of opcode" << std::hex << (int) opcode << std::endl;
    }
    return ret;
}


}
