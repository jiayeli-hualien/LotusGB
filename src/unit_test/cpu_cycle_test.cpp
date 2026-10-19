#include <array>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

#include "GBCPU.hpp"

namespace {

using namespace LOTUSGB;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class TestMemory : public IMemoryAccess {
public:
    uint8_t read(uint16_t addr) override {
        reads.push_back(addr);
        return bytes[addr];
    }

    void write(uint16_t addr, uint8_t value) override {
        writes.emplace_back(addr, value);
        bytes[addr] = value;
    }

    void resize(size_t) override {}

    std::array<uint8_t, 65536> bytes{};
    std::vector<uint16_t> reads;
    std::vector<std::pair<uint16_t, uint8_t>> writes;
};

class TestDecoder : public IDecoder {
public:
    explicit TestDecoder(IInstruction *instruction) : instruction(instruction) {}

    IInstruction *decode(const uint8_t &opcode) override {
        decodedOpcodes.push_back(opcode);
        return instruction;
    }

    IInstruction *instruction;
    std::vector<uint8_t> decodedOpcodes;
};

class IncrementAccumulator : public IInstruction {
public:
    bool stepOneMemCycle(InstState *state, Reg *reg) override {
        ++reg->getRefA();
        state->memMode = MEM_MODE_FETCH;
        return true;
    }
};

class WriteThenFetch : public IInstruction {
public:
    bool stepOneMemCycle(InstState *state, Reg *) override {
        if (!hasWritten) {
            state->memAddr = 0xC000;
            state->memValue = 0x42;
            state->memMode = MEM_MODE_WRITE;
            hasWritten = true;
        } else {
            state->memMode = MEM_MODE_FETCH;
        }
        return true;
    }

private:
    bool hasWritten = false;
};

void testFetchAndInstructionStep() {
    TestMemory memory;
    memory.bytes[0] = 0x00;
    memory.bytes[1] = 0x01;

    IncrementAccumulator instruction;
    TestDecoder baseDecoder(&instruction);
    TestDecoder cbDecoder(&instruction);
    GBCPU cpu(&memory, &baseDecoder, &cbDecoder);

    cpu.reset();
    const RetStepOneCycle result = cpu.stepOneCycle();
    Reg reg = cpu.getReg();

    expect(result.success, "CPU accepts a valid instruction cycle");
    expect(result.fetchedNewInst, "CPU reports completion when it fetches the next opcode");
    expect(reg.getRefA() == 1, "the decoded instruction updates CPU state");
    expect(reg.getRefPC() == 2, "reset and the next-opcode fetch advance PC");
    expect(memory.reads.size() == 2, "reset and instruction completion each read the opcode");
    expect(baseDecoder.decodedOpcodes == std::vector<uint8_t>({0x00, 0x01}),
           "base decoder receives fetched opcodes in order");
}

void testMemoryWriteThroughInterface() {
    TestMemory memory;
    memory.bytes[0] = 0x00;
    memory.bytes[1] = 0x01;

    WriteThenFetch instruction;
    TestDecoder baseDecoder(&instruction);
    TestDecoder cbDecoder(&instruction);
    GBCPU cpu(&memory, &baseDecoder, &cbDecoder);
    cpu.reset();

    const RetStepOneCycle writeResult = cpu.stepOneCycle();
    expect(writeResult.success, "CPU accepts a memory-write cycle");
    expect(!writeResult.fetchedNewInst, "an intermediate write cycle is not a new instruction");
    expect(memory.writes == std::vector<std::pair<uint16_t, uint8_t>>({{0xC000, 0x42}}),
           "CPU forwards the instruction's write to memory");

    const RetStepOneCycle fetchResult = cpu.stepOneCycle();
    expect(fetchResult.success && fetchResult.fetchedNewInst,
           "CPU fetches the next opcode after the instruction completes");
}

} // namespace

int main() {
    testFetchAndInstructionStep();
    testMemoryWriteThroughInterface();
    std::cout << "CPU interface tests passed\n";
    return EXIT_SUCCESS;
}
