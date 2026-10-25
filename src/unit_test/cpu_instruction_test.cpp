#include <array>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <utility>
#include <vector>

#include "GBCPU.hpp"
#include "decoder.hpp"
#include "interrupt_registers.hpp"

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

struct CpuFixture {
    TestMemory memory;
    InterruptRegisters interrupts;
    Decoder baseDecoder{false};
    Decoder cbDecoder{true};
    GBCPU cpu{memory, baseDecoder, cbDecoder, &interrupts};

    explicit CpuFixture(std::initializer_list<std::pair<uint16_t, uint8_t>> program) {
        for (const auto &[addr, byte] : program)
            memory.bytes[addr] = byte;
        cpu.reset();
        expect(cpu.stepOneCycle().fetchedNewInst,
               "fixture performs the initial opcode fetch in a CPU cycle");
        memory.reads.clear();
    }

    unsigned runUntilNextOpcode(unsigned maxCycles = 8) {
        for (unsigned cycle = 1; cycle <= maxCycles; ++cycle) {
            const auto result = cpu.stepOneCycle();
            expect(result.success, "instruction cycle succeeds");
            if (result.fetchedNewInst)
                return cycle;
        }
        expect(false, "instruction fetch completes within expected cycles");
        return 0;
    }
};

void setFlags(Reg &reg, bool z, bool n, bool h, bool c) {
    GB_Flag flags;
    flags.Z = z;
    flags.N = n;
    flags.H = h;
    flags.C = c;
    reg.setFlag(flags);
}

uint16_t& commonPairRef(Reg &reg, unsigned pairIndex) {
    switch (pairIndex) {
        case 0: return reg.getRefBC();
        case 1: return reg.getRefDE();
        case 2: return reg.getRefHL();
        default: return reg.getRefSP();
    }
}

uint8_t& byteRegisterRef(Reg &reg, unsigned index) {
    switch (index) {
        case 0: return reg.getRefB();
        case 1: return reg.getRefC();
        case 2: return reg.getRefD();
        case 3: return reg.getRefE();
        case 4: return reg.getRefH();
        case 5: return reg.getRefL();
        case 7: return reg.getRefA();
        default:
            expect(false, "test selects a valid 8-bit register");
            return reg.getRefA();
    }
}

void testCbRegisterOperationsCompleteAfterPrefixCycle() {
    struct Case {
        uint8_t cbOpcode;
        uint8_t initialB;
        bool initialZ;
        bool initialN;
        bool initialH;
        bool initialC;
        uint8_t expectedB;
        bool z;
        bool n;
        bool h;
        bool c;
    };
    const Case cases[] = {
        {0x00, 0x81, false, true,  true,  false, 0x03, false, false, false, true},  // RLC B
        {0x08, 0x01, true,  true,  true,  false, 0x80, false, false, false, true},  // RRC B
        {0x10, 0x80, true,  true,  true,  true,  0x01, false, false, false, true},  // RL B
        {0x18, 0x01, false, true,  true,  true,  0x80, false, false, false, true},  // RR B
        {0x20, 0x80, false, true,  true,  false, 0x00, true,  false, false, true},  // SLA B
        {0x28, 0x81, true,  true,  true,  false, 0xC0, false, false, false, true},  // SRA B
        {0x30, 0xF0, true,  true,  true,  true,  0x0F, false, false, false, false}, // SWAP B
        {0x38, 0x01, false, true,  true,  false, 0x00, true,  false, false, true},  // SRL B
        {0x40, 0x00, false, true,  false, true,  0x00, true,  false, true,  true},  // BIT 0,B
        {0x78, 0x80, true,  true,  false, false, 0x80, false, false, true,  false}, // BIT 7,B
        {0x80, 0xFF, true,  true,  true,  false, 0xFE, true,  true,  true,  false}, // RES 0,B
        {0xF8, 0x00, false, true,  false, true,  0x80, false, true,  false, true},  // SET 7,B
    };

    for (const Case &test : cases) {
        CpuFixture f{{0x0000, 0xCB}, {0x0001, test.cbOpcode}, {0x0002, 0x00}};
        Reg reg = f.cpu.getReg();
        reg.getRefB() = test.initialB;
        setFlags(reg, test.initialZ, test.initialN, test.initialH, test.initialC);
        f.cpu.setReg(reg);

        const auto cbOpcodeFetch = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        GB_Flag flags = reg.getFlag();
        expect(cbOpcodeFetch.success && !cbOpcodeFetch.fetchedNewInst &&
                   cbOpcodeFetch.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == std::vector<uint16_t>{0x0001} &&
                   f.memory.writes.empty() && reg.getRefB() == test.initialB &&
                   reg.getRefPC() == 2 && flags.Z == test.initialZ &&
                   flags.N == test.initialN && flags.H == test.initialH &&
                   flags.C == test.initialC,
               "CB register opcode fetch leaves the operand register and flags unchanged");

        const auto operation = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        flags = reg.getFlag();
        expect(operation.success && operation.fetchedNewInst &&
                   operation.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == std::vector<uint16_t>({0x0001, 0x0002}) &&
                   f.memory.writes.empty() && reg.getRefB() == test.expectedB &&
                   reg.getRefPC() == 3,
               "CB register operation commits with the following opcode fetch");
        expect(flags.Z == test.z && flags.N == test.n && flags.H == test.h &&
                   flags.C == test.c,
               "CB register operation applies its documented flag behavior");
    }
}

void testBitHlUsesReadAndNextFetchCycles() {
    for (unsigned bit = 0; bit < 8; ++bit) {
        for (const bool bitIsSet : {false, true}) {
            CpuFixture f{{0x0000, 0xCB},
                         {0x0001, static_cast<uint8_t>(0x46 + (bit << 3))},
                         {0x0002, 0x00}, {0xC000, static_cast<uint8_t>(
                             bitIsSet ? (1U << bit) : 0U)}};
            Reg reg = f.cpu.getReg();
            reg.getRefH() = 0xC0;
            reg.getRefL() = 0x00;
            setFlags(reg, true, true, false, true);
            f.cpu.setReg(reg);

            const auto cbOpcode = f.cpu.stepOneCycle();
            expect(cbOpcode.success && !cbOpcode.fetchedNewInst &&
                       f.memory.reads == std::vector<uint16_t>{0x0001},
                   "BIT b,(HL) fetches its CB opcode in the second M-cycle");

            const auto dataRead = f.cpu.stepOneCycle();
            reg = f.cpu.getReg();
            GB_Flag flags = reg.getFlag();
            expect(dataRead.success && !dataRead.fetchedNewInst &&
                       f.memory.reads == std::vector<uint16_t>{0x0001, 0xC000} &&
                       f.memory.writes.empty() && flags.Z && flags.N && !flags.H && flags.C,
                   "BIT b,(HL) reads memory without changing flags or writing it in M3");

            const auto nextOpcode = f.cpu.stepOneCycle();
            reg = f.cpu.getReg();
            flags = reg.getFlag();
            const bool expectedZero = !bitIsSet;
            expect(nextOpcode.success && nextOpcode.fetchedNewInst &&
                       nextOpcode.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                       f.memory.reads == std::vector<uint16_t>{0x0001, 0xC000, 0x0002} &&
                       f.memory.writes.empty() && reg.getRefHL() == 0xC000,
                   "BIT b,(HL) fetches the next opcode after exactly three M-cycles");
            expect(flags.Z == expectedZero && !flags.N && flags.H && flags.C,
                   "BIT b,(HL) updates Z/N/H and preserves C at the next-fetch boundary");
        }
    }
}

void testIncDecHlReadModifyWritePhases() {
    struct Case {
        uint8_t opcode;
        uint8_t input;
        uint8_t expected;
        bool z;
        bool n;
        bool h;
    };
    const Case cases[] = {
        {0x34, 0x0F, 0x10, false, false, true},
        {0x34, 0xFF, 0x00, true, false, true},
        {0x35, 0x10, 0x0F, false, true, true},
        {0x35, 0x01, 0x00, true, true, false},
    };

    for (const Case &test : cases) {
        CpuFixture f{{0x0000, test.opcode}, {0x0001, 0x00},
                     {0xC000, test.input}};
        Reg reg = f.cpu.getReg();
        reg.getRefH() = 0xC0;
        reg.getRefL() = 0x00;
        setFlags(reg, true, true, true, true);
        f.cpu.setReg(reg);

        const auto dataRead = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        GB_Flag flags = reg.getFlag();
        expect(dataRead.success && !dataRead.fetchedNewInst &&
                   f.memory.reads == std::vector<uint16_t>{0xC000} &&
                   f.memory.writes.empty() && f.memory.bytes[0xC000] == test.input &&
                   flags.Z && flags.N && flags.H && flags.C,
               "INC/DEC (HL) reads data without writing or updating flags in M2");

        const auto dataWrite = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        flags = reg.getFlag();
        expect(dataWrite.success && !dataWrite.fetchedNewInst &&
                   f.memory.writes ==
                       std::vector<std::pair<uint16_t, uint8_t>>{{0xC000, test.expected}} &&
                   f.memory.bytes[0xC000] == test.expected &&
                   flags.Z == test.z && flags.N == test.n && flags.H == test.h && flags.C,
               "INC/DEC (HL) writes the result and updates flags together in M3");

        const auto nextOpcode = f.cpu.stepOneCycle();
        expect(nextOpcode.success && nextOpcode.fetchedNewInst &&
                   f.memory.reads == std::vector<uint16_t>{0xC000, 0x0001} &&
                   f.memory.writes.size() == 1,
               "INC/DEC (HL) fetches the next opcode after three M-cycles");
    }
}

void testRegisterIncDecCompletesInOneCycle() {
    struct Case {
        uint8_t opcode;
        unsigned registerIndex;
        uint8_t input;
        uint8_t expected;
        bool z;
        bool n;
        bool h;
        bool carry;
    };
    const Case cases[] = {
        {0x04, 0, 0x0F, 0x10, false, false, true,  true},  // INC B, half-carry
        {0x0C, 1, 0xFF, 0x00, true,  false, true,  false}, // INC C, wrap and zero
        {0x14, 2, 0x00, 0x01, false, false, false, true},  // INC D, preserve carry
        {0x1C, 3, 0xFE, 0xFF, false, false, false, false}, // INC E
        {0x24, 4, 0x0F, 0x10, false, false, true,  true},  // INC H
        {0x2C, 5, 0xFF, 0x00, true,  false, true,  false}, // INC L
        {0x3C, 7, 0x00, 0x01, false, false, false, true},  // INC A
        {0x05, 0, 0x10, 0x0F, false, true,  true,  true},  // DEC B, half-borrow
        {0x0D, 1, 0x00, 0xFF, false, true,  true,  false}, // DEC C, wrap
        {0x15, 2, 0x01, 0x00, true,  true,  false, true},  // DEC D, zero
        {0x1D, 3, 0xFF, 0xFE, false, true,  false, false}, // DEC E
        {0x25, 4, 0x10, 0x0F, false, true,  true,  true},  // DEC H
        {0x2D, 5, 0x01, 0x00, true,  true,  false, false}, // DEC L
        {0x3D, 7, 0x00, 0xFF, false, true,  true,  true},  // DEC A
    };

    for (const Case &test : cases) {
        CpuFixture f{{0x0000, test.opcode}, {0x0001, 0x00}};
        Reg reg = f.cpu.getReg();
        byteRegisterRef(reg, test.registerIndex) = test.input;
        setFlags(reg, true, true, true, test.carry);
        f.cpu.setReg(reg);

        const auto operation = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        const GB_Flag flags = reg.getFlag();
        expect(operation.success && operation.fetchedNewInst &&
                   operation.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == std::vector<uint16_t>{0x0001} &&
                   f.memory.writes.empty() &&
                   byteRegisterRef(reg, test.registerIndex) == test.expected &&
                   reg.getRefPC() == 2,
               "INC/DEC register updates in one cycle with only the next opcode fetch");
        expect(flags.Z == test.z && flags.N == test.n && flags.H == test.h &&
                   flags.C == test.carry,
               "INC/DEC register updates Z/N/H and preserves C in one cycle");
    }
}

void testRegisterToRegisterLoadsCompleteInOneCycle() {
    constexpr unsigned registerIndexes[] = {0, 1, 2, 3, 4, 5, 7};
    constexpr uint8_t initialValues[] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x00, 0x77};

    for (const unsigned destination : registerIndexes) {
        for (const unsigned source : registerIndexes) {
            const uint8_t opcode = static_cast<uint8_t>(
                0x40U | (destination << 3U) | source);
            CpuFixture f{{0x0000, opcode}, {0x0001, 0x00}};
            Reg reg = f.cpu.getReg();
            for (const unsigned index : registerIndexes)
                byteRegisterRef(reg, index) = initialValues[index];
            setFlags(reg, true, true, true, true);
            f.cpu.setReg(reg);

            const auto load = f.cpu.stepOneCycle();
            reg = f.cpu.getReg();
            expect(load.success && load.fetchedNewInst &&
                       load.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                       f.memory.reads == std::vector<uint16_t>{0x0001} &&
                       f.memory.writes.empty() && reg.getRefPC() == 2,
                   "LD r,r completes in one cycle with only the next opcode fetch");

            for (const unsigned index : registerIndexes) {
                const uint8_t expected = index == destination
                    ? initialValues[source] : initialValues[index];
                expect(byteRegisterRef(reg, index) == expected,
                       "LD r,r changes only its destination register");
            }
            const GB_Flag flags = reg.getFlag();
            expect(flags.Z && flags.N && flags.H && flags.C,
                   "LD r,r preserves all flags");
        }
    }
}

void testSingleCycleFlagAndAccumulatorOperations() {
    struct Case {
        uint8_t opcode;
        uint8_t accumulator;
        bool initialZ;
        bool initialN;
        bool initialH;
        bool initialC;
        uint8_t expectedAccumulator;
        bool z;
        bool n;
        bool h;
        bool c;
    };
    const Case cases[] = {
        {0x3F, 0x55, true,  true,  true,  false, 0x55, true,  false, false, true},  // CCF: 0 -> 1
        {0x3F, 0x55, false, true,  true,  true,  0x55, false, false, false, false}, // CCF: 1 -> 0
        {0x37, 0x55, true,  true,  true,  false, 0x55, true,  false, false, true},  // SCF
        {0x2F, 0x55, true,  false, false, true,  0xAA, true,  true,  true,  true},  // CPL
        {0x07, 0x80, true,  true,  true,  false, 0x01, false, false, false, true},  // RLCA
        {0x07, 0x00, true,  true,  true,  true,  0x00, false, false, false, false}, // RLCA zero result
        {0x17, 0x80, false, true,  true,  true,  0x01, false, false, false, true},  // RLA carries in
        {0x17, 0x80, true,  true,  true,  false, 0x00, false, false, false, true},  // RLA carries out
        {0x0F, 0x01, true,  true,  true,  true,  0x80, false, false, false, true},  // RRCA
        {0x1F, 0x00, true,  true,  true,  true,  0x80, false, false, false, false}, // RRA carries in
        {0x1F, 0x01, true,  true,  true,  false, 0x00, false, false, false, true},  // RRA carries out
    };

    for (const Case &test : cases) {
        CpuFixture f{{0x0000, test.opcode}, {0x0001, 0x00}};
        Reg reg = f.cpu.getReg();
        reg.getRefA() = test.accumulator;
        setFlags(reg, test.initialZ, test.initialN, test.initialH, test.initialC);
        f.cpu.setReg(reg);

        const auto operation = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        const GB_Flag flags = reg.getFlag();
        expect(operation.success && operation.fetchedNewInst &&
                   operation.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == std::vector<uint16_t>{0x0001} &&
                   f.memory.writes.empty() && reg.getRefA() == test.expectedAccumulator &&
                   reg.getRefPC() == 2,
               "flag and accumulator operation completes with the next fetch in one cycle");
        expect(flags.Z == test.z && flags.N == test.n && flags.H == test.h &&
                   flags.C == test.c,
               "flag and accumulator operation applies the documented flag behavior");
    }
}

void testCbHlReadModifyWritePhases() {
    struct Case {
        uint8_t opcode;
        uint8_t input;
        uint8_t expected;
        bool changesFlags;
    };
    const Case cases[] = {
        {0x06, 0x81, 0x03, true}, // RLC (HL)
        {0x86, 0xFF, 0xFE, false}, // RES 0,(HL)
        {0xC6, 0xFE, 0xFF, false}, // SET 0,(HL)
    };

    for (const Case &test : cases) {
        CpuFixture f{{0x0000, 0xCB}, {0x0001, test.opcode}, {0x0002, 0x00},
                     {0xC000, test.input}};
        Reg reg = f.cpu.getReg();
        reg.getRefH() = 0xC0;
        reg.getRefL() = 0x00;
        setFlags(reg, true, true, true, true);
        f.cpu.setReg(reg);

        const auto cbOpcode = f.cpu.stepOneCycle();
        expect(cbOpcode.success && !cbOpcode.fetchedNewInst &&
                   f.memory.reads == std::vector<uint16_t>{0x0001} &&
                   f.memory.writes.empty(),
               "CB read-modify-write fetches its extended opcode before touching (HL)");

        const auto dataRead = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        GB_Flag flags = reg.getFlag();
        expect(dataRead.success && !dataRead.fetchedNewInst &&
                   f.memory.reads == std::vector<uint16_t>{0x0001, 0xC000} &&
                   f.memory.writes.empty() && f.memory.bytes[0xC000] == test.input &&
                   flags.Z && flags.N && flags.H && flags.C,
               "CB read-modify-write reads (HL) before its ALU/write phase");

        const auto dataWrite = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        flags = reg.getFlag();
        expect(dataWrite.success && !dataWrite.fetchedNewInst &&
                   f.memory.writes ==
                       std::vector<std::pair<uint16_t, uint8_t>>{{0xC000, test.expected}} &&
                   f.memory.bytes[0xC000] == test.expected,
               "CB read-modify-write commits the transformed byte in M4");
        if (test.changesFlags) {
            expect(!flags.Z && !flags.N && !flags.H && flags.C,
                   "CB rotate updates flags during the memory write phase");
        } else {
            expect(flags.Z && flags.N && flags.H && flags.C,
                   "CB RES/SET preserve flags during the memory write phase");
        }

        const auto nextOpcode = f.cpu.stepOneCycle();
        expect(nextOpcode.success && nextOpcode.fetchedNewInst &&
                   f.memory.reads == std::vector<uint16_t>{0x0001, 0xC000, 0x0002} &&
                   f.memory.writes.size() == 1,
               "CB read-modify-write fetches the next opcode after four M-cycles");
    }
}

void testHlAluReadsBeforeUpdatingAccumulatorAndFlags() {
    struct Case {
        uint8_t opcode;
        uint8_t accumulator;
        uint8_t memoryValue;
        uint8_t expectedAccumulator;
        bool z;
        bool n;
        bool h;
        bool c;
    };
    const Case cases[] = {
        {0x86, 0xFF, 0x01, 0x00, true, false, true, true},  // ADD A,(HL)
        {0x8E, 0x0F, 0x00, 0x10, false, false, true, false}, // ADC A,(HL), C=1
        {0x96, 0x00, 0x01, 0xFF, false, true, true, true},  // SUB (HL)
        {0x9E, 0x10, 0x00, 0x0F, false, true, true, false}, // SBC A,(HL), C=1
        {0xA6, 0xF0, 0x0F, 0x00, true, false, true, false}, // AND (HL)
        {0xAE, 0xAA, 0xAA, 0x00, true, false, false, false}, // XOR (HL)
        {0xB6, 0x80, 0x01, 0x81, false, false, false, false}, // OR (HL)
        {0xBE, 0x10, 0x10, 0x10, true, true, false, false}, // CP (HL)
    };

    for (const Case &test : cases) {
        CpuFixture f{{0x0000, test.opcode}, {0x0001, 0x00},
                     {0xC000, test.memoryValue}};
        Reg reg = f.cpu.getReg();
        reg.getRefA() = test.accumulator;
        reg.getRefH() = 0xC0;
        reg.getRefL() = 0x00;
        setFlags(reg, true, true, true, true);
        f.cpu.setReg(reg);

        const auto dataRead = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        GB_Flag flags = reg.getFlag();
        expect(dataRead.success && !dataRead.fetchedNewInst &&
                   f.memory.reads == std::vector<uint16_t>{0xC000} &&
                   f.memory.writes.empty() && reg.getRefA() == test.accumulator &&
                   flags.Z && flags.N && flags.H && flags.C,
               "ALU (HL) reads its operand before changing A or flags in M2");

        const auto nextOpcode = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        flags = reg.getFlag();
        expect(nextOpcode.success && nextOpcode.fetchedNewInst &&
                   f.memory.reads == std::vector<uint16_t>{0xC000, 0x0001} &&
                   f.memory.writes.empty() && reg.getRefA() == test.expectedAccumulator &&
                   reg.getRefHL() == 0xC000 && reg.getRefPC() == 2,
               "ALU (HL) updates A with the next-opcode fetch in M3");
        expect(flags.Z == test.z && flags.N == test.n && flags.H == test.h &&
                   flags.C == test.c,
               "ALU (HL) commits the operation-specific flags at the final phase");
    }
}

void testImmediateAluReadsBeforeUpdatingAccumulatorAndFlags() {
    struct Case {
        uint8_t opcode;
        uint8_t accumulator;
        uint8_t immediate;
        bool carryIn;
        uint8_t expectedAccumulator;
        bool z;
        bool n;
        bool h;
        bool c;
    };
    const Case cases[] = {
        {0xC6, 0xFF, 0x01, false, 0x00, true,  false, true,  true},  // ADD A,n
        {0xCE, 0xFF, 0x00, true,  0x00, true,  false, true,  true},  // ADC A,n
        {0xD6, 0x00, 0x01, false, 0xFF, false, true,  true,  true},  // SUB A,n
        {0xDE, 0x00, 0x00, true,  0xFF, false, true,  true,  true},  // SBC A,n
        {0xE6, 0xF0, 0x0F, false, 0x00, true,  false, true,  false}, // AND A,n
        {0xEE, 0xAA, 0xAA, false, 0x00, true,  false, false, false}, // XOR A,n
        {0xF6, 0x80, 0x01, false, 0x81, false, false, false, false}, // OR A,n
        {0xFE, 0x10, 0x10, false, 0x10, true,  true,  false, false}, // CP A,n
        {0xFE, 0x10, 0x11, false, 0x10, false, true,  true,  true},  // CP A,n borrow
    };

    for (const Case &test : cases) {
        CpuFixture f{{0x0000, test.opcode}, {0x0001, test.immediate}, {0x0002, 0x00}};
        Reg reg = f.cpu.getReg();
        reg.getRefA() = test.accumulator;
        setFlags(reg, true, true, true, test.carryIn);
        f.cpu.setReg(reg);

        const auto immediateRead = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        GB_Flag flags = reg.getFlag();
        expect(immediateRead.success && !immediateRead.fetchedNewInst &&
                   immediateRead.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == std::vector<uint16_t>{0x0001} &&
                   f.memory.writes.empty() && reg.getRefA() == test.accumulator &&
                   reg.getRefPC() == 2 && flags.Z && flags.N && flags.H &&
                   flags.C == test.carryIn,
               "immediate ALU reads n in M2 before changing A or flags");

        const auto nextOpcode = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        flags = reg.getFlag();
        expect(nextOpcode.success && nextOpcode.fetchedNewInst &&
                   nextOpcode.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == std::vector<uint16_t>({0x0001, 0x0002}) &&
                   f.memory.writes.empty() &&
                   reg.getRefA() == test.expectedAccumulator && reg.getRefPC() == 3,
               "immediate ALU commits its result while fetching the next opcode in M3");
        expect(flags.Z == test.z && flags.N == test.n && flags.H == test.h &&
                   flags.C == test.c,
               "immediate ALU commits operation flags at the next-fetch boundary");
    }
}

void testRegisterAluCompletesInOneCycle() {
    struct Case {
        uint8_t opcode;
        uint8_t accumulator;
        uint8_t source;
        bool carryIn;
        uint8_t expectedAccumulator;
        bool z;
        bool n;
        bool h;
        bool c;
    };
    const Case cases[] = {
        {0x80, 0xFF, 0x01, false, 0x00, true,  false, true,  true},  // ADD A,B
        {0x88, 0xFF, 0x00, true,  0x00, true,  false, true,  true},  // ADC A,B
        {0x90, 0x00, 0x01, false, 0xFF, false, true,  true,  true},  // SUB B
        {0x98, 0x00, 0x00, true,  0xFF, false, true,  true,  true},  // SBC B
        {0xA0, 0xF0, 0x0F, false, 0x00, true,  false, true,  false}, // AND B
        {0xA8, 0xAA, 0xAA, false, 0x00, true,  false, false, false}, // XOR B
        {0xB0, 0x80, 0x01, false, 0x81, false, false, false, false}, // OR B
        {0xB8, 0x10, 0x11, false, 0x10, false, true,  true,  true},  // CP B
    };

    for (const Case &test : cases) {
        CpuFixture f{{0x0000, test.opcode}, {0x0001, 0x00}};
        Reg reg = f.cpu.getReg();
        reg.getRefA() = test.accumulator;
        reg.getRefB() = test.source;
        setFlags(reg, true, true, true, test.carryIn);
        f.cpu.setReg(reg);

        const auto operation = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        const GB_Flag flags = reg.getFlag();
        expect(operation.success && operation.fetchedNewInst &&
                   operation.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == std::vector<uint16_t>{0x0001} &&
                   f.memory.writes.empty() && reg.getRefA() == test.expectedAccumulator &&
                   reg.getRefPC() == 2,
               "register ALU applies its result in one cycle with the next opcode fetch");
        expect(flags.Z == test.z && flags.N == test.n && flags.H == test.h &&
                   flags.C == test.c,
               "register ALU commits the operation-specific flags in its single cycle");
    }
}

void testIndirectLoadsReadOldAddressAndUpdateAtCorrectPhase() {
    struct Case {
        uint8_t opcode;
        uint16_t initialPair;
        uint16_t readAddress;
        uint16_t expectedPair;
        uint8_t memoryValue;
    };
    const Case cases[] = {
        {0x0A, 0xC001, 0xC001, 0xC001, 0xA5}, // LD A,(BC)
        {0x1A, 0xC002, 0xC002, 0xC002, 0x5A}, // LD A,(DE)
        {0x2A, 0xFFFF, 0xFFFF, 0x0000, 0xA5}, // LD A,(HL+), 16-bit wrap
        {0x3A, 0x0000, 0x0000, 0xFFFF, 0x3A}, // LD A,(HL-), opcode byte at address 0
    };

    for (const Case &test : cases) {
        CpuFixture f{{0x0000, test.opcode}, {0x0001, 0x00}};
        Reg reg = f.cpu.getReg();
        reg.getRefA() = 0x42;
        if (test.opcode == 0x0A)
            reg.getRefBC() = test.initialPair;
        else if (test.opcode == 0x1A)
            reg.getRefDE() = test.initialPair;
        else
            reg.getRefHL() = test.initialPair;
        setFlags(reg, true, true, true, true);
        f.cpu.setReg(reg);
        f.memory.bytes[test.readAddress] = test.memoryValue;

        const auto dataRead = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        GB_Flag flags = reg.getFlag();
        expect(dataRead.success && !dataRead.fetchedNewInst &&
                   f.memory.reads == std::vector<uint16_t>{test.readAddress} &&
                   f.memory.writes.empty() && reg.getRefA() == 0x42 &&
                   reg.getRefPC() == 1 && flags.Z && flags.N && flags.H && flags.C,
               "indirect A load reads through the old address before committing A");
        if (test.opcode == 0x0A) {
            expect(reg.getRefBC() == test.expectedPair,
                   "LD A,(BC) leaves BC unchanged after the data read");
        } else if (test.opcode == 0x1A) {
            expect(reg.getRefDE() == test.expectedPair,
                   "LD A,(DE) leaves DE unchanged after the data read");
        } else {
            expect(reg.getRefHL() == test.expectedPair,
                   "LD A,(HL+/-) updates HL during the memory-read M-cycle");
        }

        const auto nextOpcode = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        flags = reg.getFlag();
        expect(nextOpcode.success && nextOpcode.fetchedNewInst &&
                   f.memory.reads ==
                       std::vector<uint16_t>{test.readAddress, 0x0001} &&
                   f.memory.writes.empty() && reg.getRefA() == test.memoryValue &&
                   reg.getRefPC() == 2 && flags.Z && flags.N && flags.H && flags.C,
               "indirect A load commits data while fetching the next opcode in M3");
        if (test.opcode == 0x0A) {
            expect(reg.getRefBC() == test.expectedPair,
                   "LD A,(BC) preserves BC through completion");
        } else if (test.opcode == 0x1A) {
            expect(reg.getRefDE() == test.expectedPair,
                   "LD A,(DE) preserves DE through completion");
        } else {
            expect(reg.getRefHL() == test.expectedPair,
                   "LD A,(HL+/-) retains its incremented or decremented HL");
        }
    }
}

void testIndirectStoresWriteOldAddressAndUpdateAtCorrectPhase() {
    struct Case {
        uint8_t opcode;
        uint16_t initialPair;
        uint16_t writeAddress;
        uint16_t expectedPair;
    };
    const Case cases[] = {
        {0x02, 0xC001, 0xC001, 0xC001}, // LD (BC),A
        {0x12, 0xC002, 0xC002, 0xC002}, // LD (DE),A
        {0x22, 0xFFFF, 0xFFFF, 0x0000}, // LD (HL+),A, 16-bit wrap
        {0x32, 0x0000, 0x0000, 0xFFFF}, // LD (HL-),A, 16-bit wrap
    };

    for (const Case &test : cases) {
        CpuFixture f{{0x0000, test.opcode}, {0x0001, 0x00}};
        Reg reg = f.cpu.getReg();
        reg.getRefA() = 0x96;
        if (test.opcode == 0x02)
            reg.getRefBC() = test.initialPair;
        else if (test.opcode == 0x12)
            reg.getRefDE() = test.initialPair;
        else
            reg.getRefHL() = test.initialPair;
        setFlags(reg, true, true, true, true);
        f.cpu.setReg(reg);

        const auto dataWrite = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        GB_Flag flags = reg.getFlag();
        expect(dataWrite.success && !dataWrite.fetchedNewInst &&
                   dataWrite.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads.empty() &&
                   f.memory.writes ==
                       std::vector<std::pair<uint16_t, uint8_t>>{{test.writeAddress, 0x96}} &&
                   f.memory.bytes[test.writeAddress] == 0x96 && reg.getRefA() == 0x96 &&
                   reg.getRefPC() == 1 && flags.Z && flags.N && flags.H && flags.C,
               "indirect A store writes A to the original address in M2 without changing flags");
        if (test.opcode == 0x02) {
            expect(reg.getRefBC() == test.expectedPair,
                   "LD (BC),A leaves BC unchanged during its data-write phase");
        } else if (test.opcode == 0x12) {
            expect(reg.getRefDE() == test.expectedPair,
                   "LD (DE),A leaves DE unchanged during its data-write phase");
        } else {
            expect(reg.getRefHL() == test.expectedPair,
                   "LD (HL+/-),A updates HL in the data-write M-cycle");
        }

        const auto nextOpcode = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        flags = reg.getFlag();
        expect(nextOpcode.success && nextOpcode.fetchedNewInst &&
                   nextOpcode.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == std::vector<uint16_t>{0x0001} &&
                   f.memory.writes.size() == 1 && reg.getRefA() == 0x96 &&
                   reg.getRefPC() == 2 && flags.Z && flags.N && flags.H && flags.C,
               "indirect A store fetches the next opcode after its M2 write");
        if (test.opcode == 0x02) {
            expect(reg.getRefBC() == test.expectedPair,
                   "LD (BC),A preserves BC through completion");
        } else if (test.opcode == 0x12) {
            expect(reg.getRefDE() == test.expectedPair,
                   "LD (DE),A preserves DE through completion");
        } else {
            expect(reg.getRefHL() == test.expectedPair,
                   "LD (HL+/-),A retains the updated HL through completion");
        }
    }
}

void testDaaBoundaryCases() {
    struct Case { uint8_t a; bool n; bool h; bool c; uint8_t result; bool z; bool outC; };
    const Case cases[] = {
        {0x9A, false, false, false, 0x00, true, true},
        {0x0A, false, true, false, 0x10, false, false},
        {0x13, true, true, false, 0x0D, false, false},
        {0x00, true, false, true, 0xA0, false, true},
    };

    for (const auto &test : cases) {
        CpuFixture f{{0x0000, 0x27}, {0x0001, 0x00}};
        Reg reg = f.cpu.getReg();
        reg.getRefA() = test.a;
        setFlags(reg, false, test.n, test.h, test.c);
        f.cpu.setReg(reg);
        expect(f.runUntilNextOpcode() == 1, "DAA takes one M-cycle");
        reg = f.cpu.getReg();
        const GB_Flag flags = reg.getFlag();
        expect(reg.getRefA() == test.result, "DAA adjusts accumulator at boundary values");
        expect(flags.Z == test.z && flags.C == test.outC && !flags.H,
               "DAA updates Z/H/C correctly");
    }

    CpuFixture exhaustive{{0x0000, 0x27}, {0x0001, 0x00}};
    for (unsigned a = 0; a <= 0xFF; ++a) {
        for (unsigned flagBits = 0; flagBits < 8; ++flagBits) {
            const bool n = (flagBits & 0x4) != 0;
            const bool h = (flagBits & 0x2) != 0;
            const bool c = (flagBits & 0x1) != 0;
            uint8_t adjustment = 0;
            bool expectedCarry = c;
            if (n) {
                if (h) adjustment |= 0x06;
                if (c) adjustment |= 0x60;
            } else {
                if (h || (a & 0x0F) > 0x09) adjustment |= 0x06;
                if (c || a > 0x99) {
                    adjustment |= 0x60;
                    expectedCarry = true;
                }
            }
            const uint8_t expectedA = static_cast<uint8_t>(
                n ? a - adjustment : a + adjustment);

            Reg reg{};
            reg.getRefA() = static_cast<uint8_t>(a);
            reg.getRefPC() = 0;
            setFlags(reg, false, n, h, c);
            exhaustive.cpu.setReg(reg);
            exhaustive.cpu.fetchFirstOpcode();
            expect(exhaustive.cpu.stepOneCycle().success, "DAA cycle succeeds");

            reg = exhaustive.cpu.getReg();
            const GB_Flag flags = reg.getFlag();
            expect(reg.getRefA() == expectedA, "DAA matches the correction rule for every A/N/H/C input");
            expect(flags.Z == (expectedA == 0) && flags.N == n && !flags.H &&
                       flags.C == expectedCarry,
                   "DAA preserves N, clears H, and updates Z/C for every input");
        }
    }
}

void testSpOffsetFlagsAndCycles() {
    struct Case { uint16_t sp; uint8_t offset; uint16_t result; bool h; bool c; };
    const Case cases[] = {
        {0xFFF8, 0x08, 0x0000, true, true},
        {0x0000, 0xFF, 0xFFFF, false, false},
        {0x000F, 0x01, 0x0010, true, false},
        {0x00FF, 0x01, 0x0100, true, true},
    };

    for (const auto &test : cases) {
        CpuFixture f{{0x0000, 0xE8}, {0x0001, test.offset}, {0x0002, 0x00}};
        Reg reg = f.cpu.getReg();
        reg.getRefSP() = test.sp;
        setFlags(reg, true, true, false, false);
        f.cpu.setReg(reg);
        expect(f.runUntilNextOpcode() == 4, "ADD SP,e8 takes four M-cycles");
        reg = f.cpu.getReg();
        const GB_Flag flags = reg.getFlag();
        expect(reg.getRefSP() == test.result, "ADD SP,e8 sign-extends the offset");
        expect(!flags.Z && !flags.N && flags.H == test.h && flags.C == test.c,
               "ADD SP,e8 computes flags from the unsigned offset byte");
    }

    struct HlCase { uint16_t sp; uint8_t offset; uint16_t result; bool h; bool c; };
    const HlCase hlCases[] = {
        {0xFFF8, 0x08, 0x0000, true, true},
        {0x0000, 0xFF, 0xFFFF, false, false},
        {0x000F, 0x01, 0x0010, true, false},
        {0x00FF, 0x01, 0x0100, true, true},
    };
    for (const auto &test : hlCases) {
        CpuFixture f{{0x0000, 0xF8}, {0x0001, test.offset}, {0x0002, 0x00}};
        Reg reg = f.cpu.getReg();
        reg.getRefSP() = test.sp;
        reg.getRefHL() = 0xBEEF;
        setFlags(reg, true, true, false, false);
        f.cpu.setReg(reg);

        const auto operandRead = f.cpu.stepOneCycle();
        expect(operandRead.success && !operandRead.fetchedNewInst &&
                   f.cpu.getReg().getRefHL() == 0xBEEF,
               "LD HL,SP+e8 reads the immediate before changing HL");
        const auto lowByte = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        const GB_Flag lowFlags = reg.getFlag();
        const uint16_t intermediateHL = static_cast<uint16_t>(
            0xBE00 | static_cast<uint8_t>(test.sp + test.offset));
        expect(lowByte.success && !lowByte.fetchedNewInst &&
                   reg.getRefHL() == intermediateHL && reg.getRefSP() == test.sp,
               "LD HL,SP+e8 updates L in the low-byte M-cycle");
        expect(!lowFlags.Z && !lowFlags.N && lowFlags.H == test.h &&
                   lowFlags.C == test.c,
               "LD HL,SP+e8 exposes its low-byte flags before the high-byte phase");

        const auto highByte = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        const GB_Flag flags = reg.getFlag();
        expect(highByte.success && highByte.fetchedNewInst &&
                   reg.getRefHL() == test.result && reg.getRefSP() == test.sp,
               "LD HL,SP+e8 updates H and fetches in the final M-cycle");
        expect(!flags.Z && !flags.N && flags.H == test.h && flags.C == test.c,
               "LD HL,SP+e8 preserves the low-byte H/C results");
    }

    CpuFixture exhaustive{{0x0000, 0xE8}, {0x0001, 0x00}, {0x0002, 0x00}};
    for (unsigned spLow = 0; spLow <= 0xFF; ++spLow) {
        for (unsigned offset = 0; offset <= 0xFF; ++offset) {
            const uint16_t sp = static_cast<uint16_t>(((spLow ^ offset) << 8) | spLow);
            const int signedOffset = offset < 0x80 ? offset : (int)offset - 0x100;
            const uint16_t expectedResult = static_cast<uint16_t>(sp + signedOffset);
            const bool expectedHalfCarry = (sp & 0x0F) + (offset & 0x0F) > 0x0F;
            const bool expectedCarry = (sp & 0xFF) + offset > 0xFF;

            Reg input{};
            input.getRefSP() = sp;
            input.getRefPC() = 0;
            setFlags(input, true, true, false, false);
            exhaustive.cpu.setReg(input);
            exhaustive.memory.bytes[1] = static_cast<uint8_t>(offset);
            exhaustive.cpu.fetchFirstOpcode();
            exhaustive.memory.reads.clear();

            const auto operandRead = exhaustive.cpu.stepOneCycle();
            expect(operandRead.success && !operandRead.fetchedNewInst &&
                       exhaustive.cpu.getReg().getRefSP() == sp &&
                       exhaustive.memory.reads == std::vector<uint16_t>{1},
                   "ADD SP,e8 reads its immediate without changing SP");

            const auto lowByte = exhaustive.cpu.stepOneCycle();
            Reg intermediate = exhaustive.cpu.getReg();
            const GB_Flag intermediateFlags = intermediate.getFlag();
            expect(lowByte.success && !lowByte.fetchedNewInst &&
                       intermediate.getRefSP() == sp &&
                       !intermediateFlags.Z && !intermediateFlags.N &&
                       intermediateFlags.H == expectedHalfCarry &&
                       intermediateFlags.C == expectedCarry,
                   "ADD SP,e8 computes the low-byte flags before committing SP");

            const auto highByte = exhaustive.cpu.stepOneCycle();
            intermediate = exhaustive.cpu.getReg();
            const GB_Flag highFlags = intermediate.getFlag();
            expect(highByte.success && !highByte.fetchedNewInst &&
                       intermediate.getRefSP() == sp &&
                       highFlags.H == expectedHalfCarry &&
                       highFlags.C == expectedCarry,
                   "ADD SP,e8 computes the high byte in a separate M-cycle");

            const auto commit = exhaustive.cpu.stepOneCycle();
            expect(commit.success && commit.fetchedNewInst &&
                       exhaustive.cpu.getReg().getRefSP() == expectedResult &&
                       exhaustive.memory.reads == std::vector<uint16_t>({1, 2}),
                   "ADD SP,e8 commits the result with the next opcode fetch");

            Reg output = exhaustive.cpu.getReg();
            const GB_Flag outputFlags = output.getFlag();
            if (output.getRefSP() != expectedResult) {
                std::cerr << "ADD SP mismatch: SP=0x" << std::hex << sp
                          << " offset=0x" << offset << " expected=0x"
                          << expectedResult << " actual=0x" << output.getRefSP()
                          << std::dec << '\n';
            }
            expect(output.getRefSP() == expectedResult,
                   "ADD SP,e8 wraps the signed result to 16 bits");
            expect(!outputFlags.Z && !outputFlags.N &&
                       outputFlags.H == expectedHalfCarry &&
                       outputFlags.C == expectedCarry,
                   "ADD SP,e8 flags match all low-byte and offset combinations");
        }
    }
}

void testLdSpFromHlMcycle() {
    CpuFixture f{{0x0000, 0xF9}, {0x0001, 0x00}};
    Reg reg = f.cpu.getReg();
    reg.getRefSP() = 0x1234;
    reg.getRefHL() = 0xABCD;
    setFlags(reg, true, true, true, true);
    f.cpu.setReg(reg);

    const auto transfer = f.cpu.stepOneCycle();
    reg = f.cpu.getReg();
    const GB_Flag transferFlags = reg.getFlag();
    expect(transfer.success && !transfer.fetchedNewInst &&
               reg.getRefSP() == 0xABCD && reg.getRefHL() == 0xABCD &&
               reg.getRefPC() == 1 && f.memory.reads.empty(),
           "LD SP,HL transfers the pair during its internal M-cycle without bus access");
    expect(transferFlags.Z && transferFlags.N && transferFlags.H && transferFlags.C,
           "LD SP,HL leaves flags unchanged during the transfer");

    const auto nextFetch = f.cpu.stepOneCycle();
    reg = f.cpu.getReg();
    const GB_Flag finalFlags = reg.getFlag();
    expect(nextFetch.success && nextFetch.fetchedNewInst &&
               reg.getRefSP() == 0xABCD && reg.getRefPC() == 2 &&
               f.memory.reads == std::vector<uint16_t>{1},
           "LD SP,HL fetches the next opcode after its two-cycle sequence");
    expect(finalFlags.Z && finalFlags.N && finalFlags.H && finalFlags.C,
           "LD SP,HL preserves flags across the following opcode fetch");
}

void testJpHlFetchesAtTargetInSameMcycle() {
    CpuFixture f{{0x0000, 0xE9}, {0x1234, 0x00}};
    Reg reg = f.cpu.getReg();
    reg.getRefHL() = 0x1234;
    setFlags(reg, true, true, true, true);
    f.cpu.setReg(reg);

    const auto jump = f.cpu.stepOneCycle();
    reg = f.cpu.getReg();
    const GB_Flag flags = reg.getFlag();
    expect(jump.success && jump.fetchedNewInst &&
               jump.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               reg.getRefHL() == 0x1234 && reg.getRefPC() == 0x1235 &&
               f.memory.reads == std::vector<uint16_t>{0x1234},
           "JP HL fetches the target opcode in the jump's M-cycle");
    expect(flags.Z && flags.N && flags.H && flags.C,
           "JP HL leaves flags unchanged while redirecting the fetch");
}

void testImmediate8BitLoadPhases() {
    const uint8_t loadOpcodes[] = {0x06, 0x0E, 0x16, 0x1E, 0x26, 0x2E, 0x3E};
    const unsigned targetIndices[] = {0, 1, 2, 3, 4, 5, 7};
    const uint8_t targetOldValues[] = {0x11, 0x22, 0x33, 0x44, 0xC0, 0x00, 0x77};

    for (size_t i = 0; i < std::size(loadOpcodes); ++i) {
        CpuFixture f{{0x0000, loadOpcodes[i]}, {0x0001, 0xA6}, {0x0002, 0x00}};
        Reg reg = f.cpu.getReg();
        reg.getRefB() = 0x11;
        reg.getRefC() = 0x22;
        reg.getRefD() = 0x33;
        reg.getRefE() = 0x44;
        reg.getRefH() = 0xC0;
        reg.getRefL() = 0x00;
        reg.getRefA() = 0x77;
        setFlags(reg, true, true, true, true);
        f.cpu.setReg(reg);

        const auto immediateRead = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        GB_Flag flags = reg.getFlag();
        expect(immediateRead.success && !immediateRead.fetchedNewInst &&
                   immediateRead.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == std::vector<uint16_t>{0x0001} &&
                   f.memory.writes.empty() &&
                   byteRegisterRef(reg, targetIndices[i]) == targetOldValues[i] &&
                   reg.getRefPC() == 2 && flags.Z && flags.N && flags.H && flags.C,
               "LD r,n reads the immediate in M2 before committing the target or changing flags");

        const auto nextOpcode = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        flags = reg.getFlag();
        expect(nextOpcode.success && nextOpcode.fetchedNewInst &&
                   nextOpcode.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == std::vector<uint16_t>({0x0001, 0x0002}) &&
                   f.memory.writes.empty() &&
                   byteRegisterRef(reg, targetIndices[i]) == 0xA6 &&
                   reg.getRefPC() == 3 && flags.Z && flags.N && flags.H && flags.C,
               "LD r,n commits the target register with the following opcode fetch");
    }
}

void testImmediate16BitLoadPhases() {
    const uint8_t loadPairOpcodes[] = {0x01, 0x11, 0x21, 0x31};
    for (unsigned pairIndex = 0; pairIndex < 4; ++pairIndex) {
        CpuFixture f{{0x0000, loadPairOpcodes[pairIndex]},
                     {0x0001, 0x34}, {0x0002, 0x12}, {0x0003, 0x00}};
        Reg reg = f.cpu.getReg();
        commonPairRef(reg, pairIndex) = 0xA5A5;
        setFlags(reg, true, true, true, true);
        f.cpu.setReg(reg);

        const auto lowByte = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        expect(lowByte.success && !lowByte.fetchedNewInst &&
                   commonPairRef(reg, pairIndex) == 0xA5A5 &&
                   reg.getRefPC() == 2 && f.memory.reads == std::vector<uint16_t>{1},
               "LD rr,d16 reads the low immediate byte without committing the pair");

        const auto highByte = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        expect(highByte.success && !highByte.fetchedNewInst &&
                   commonPairRef(reg, pairIndex) == 0xA5A5 &&
                   reg.getRefPC() == 3 &&
                   f.memory.reads == std::vector<uint16_t>({1, 2}),
               "LD rr,d16 reads the high immediate byte before pair commit");

        const auto commit = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        const GB_Flag flags = reg.getFlag();
        expect(commit.success && commit.fetchedNewInst &&
                   commonPairRef(reg, pairIndex) == 0x1234 && reg.getRefPC() == 4 &&
                   f.memory.reads == std::vector<uint16_t>({1, 2, 3}),
               "LD rr,d16 commits the pair with the next opcode fetch");
        expect(flags.Z && flags.N && flags.H && flags.C,
               "LD rr,d16 preserves flags through both immediate reads and commit");
    }

    CpuFixture store{{0x0000, 0x08}, {0x0001, 0x00}, {0x0002, 0xC0},
                     {0x0003, 0x00}};
    Reg reg = store.cpu.getReg();
    reg.getRefSP() = 0xABCD;
    setFlags(reg, true, true, true, true);
    store.cpu.setReg(reg);

    const auto lowAddress = store.cpu.stepOneCycle();
    expect(lowAddress.success && !lowAddress.fetchedNewInst &&
               store.memory.writes.empty() &&
               store.memory.reads == std::vector<uint16_t>{1},
           "LD (a16),SP reads the low address byte before writing");
    const auto highAddress = store.cpu.stepOneCycle();
    expect(highAddress.success && !highAddress.fetchedNewInst &&
               store.memory.writes.empty() &&
               store.memory.reads == std::vector<uint16_t>({1, 2}),
           "LD (a16),SP reads the high address byte before either write");

    const auto lowWrite = store.cpu.stepOneCycle();
    expect(lowWrite.success && !lowWrite.fetchedNewInst &&
               store.memory.writes ==
                   std::vector<std::pair<uint16_t, uint8_t>>{{0xC000, 0xCD}},
           "LD (a16),SP writes SPL at the target in its first write M-cycle");
    const auto highWrite = store.cpu.stepOneCycle();
    expect(highWrite.success && !highWrite.fetchedNewInst &&
               store.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{
                   {0xC000, 0xCD}, {0xC001, 0xAB}},
           "LD (a16),SP writes SPH to the following address in the next M-cycle");

    const auto nextFetch = store.cpu.stepOneCycle();
    reg = store.cpu.getReg();
    const GB_Flag storeFlags = reg.getFlag();
    expect(nextFetch.success && nextFetch.fetchedNewInst && reg.getRefSP() == 0xABCD &&
               reg.getRefPC() == 4 && store.memory.reads ==
                   std::vector<uint16_t>({1, 2, 3}) &&
               store.memory.bytes[0xC000] == 0xCD &&
               store.memory.bytes[0xC001] == 0xAB,
           "LD (a16),SP fetches after its two ordered writes without changing SP");
    expect(storeFlags.Z && storeFlags.N && storeFlags.H && storeFlags.C,
           "LD (a16),SP leaves flags unchanged");
}

void testSixteenBitArithmeticMcyclePhases() {
    const uint8_t incOpcodes[] = {0x03, 0x13, 0x23, 0x33};
    const uint8_t decOpcodes[] = {0x0B, 0x1B, 0x2B, 0x3B};
    for (unsigned pairIndex = 0; pairIndex < 4; ++pairIndex) {
        CpuFixture inc{{0x0000, incOpcodes[pairIndex]}, {0x0001, 0x00}};
        Reg reg = inc.cpu.getReg();
        commonPairRef(reg, pairIndex) = 0xFFFF;
        setFlags(reg, true, true, true, true);
        inc.cpu.setReg(reg);

        const auto incInternal = inc.cpu.stepOneCycle();
        reg = inc.cpu.getReg();
        expect(incInternal.success && !incInternal.fetchedNewInst &&
                   commonPairRef(reg, pairIndex) == 0x0000 && inc.memory.reads.empty(),
               "INC rr updates through the IDU in its first internal M-cycle");
        const GB_Flag incFlags = reg.getFlag();
        expect(incFlags.Z && incFlags.N && incFlags.H && incFlags.C,
               "INC rr leaves all flags unchanged");
        const auto incFetch = inc.cpu.stepOneCycle();
        reg = inc.cpu.getReg();
        expect(incFetch.success && incFetch.fetchedNewInst &&
                   commonPairRef(reg, pairIndex) == 0x0000 &&
                   inc.memory.reads == std::vector<uint16_t>{0x0001},
               "INC rr fetches the next opcode after the IDU cycle");

        CpuFixture dec{{0x0000, decOpcodes[pairIndex]}, {0x0001, 0x00}};
        reg = dec.cpu.getReg();
        commonPairRef(reg, pairIndex) = 0x0000;
        setFlags(reg, true, true, true, true);
        dec.cpu.setReg(reg);

        const auto decInternal = dec.cpu.stepOneCycle();
        reg = dec.cpu.getReg();
        expect(decInternal.success && !decInternal.fetchedNewInst &&
                   commonPairRef(reg, pairIndex) == 0xFFFF && dec.memory.reads.empty(),
               "DEC rr updates through the IDU in its first internal M-cycle");
        const GB_Flag decFlags = reg.getFlag();
        expect(decFlags.Z && decFlags.N && decFlags.H && decFlags.C,
               "DEC rr leaves all flags unchanged");
        const auto decFetch = dec.cpu.stepOneCycle();
        reg = dec.cpu.getReg();
        expect(decFetch.success && decFetch.fetchedNewInst &&
                   commonPairRef(reg, pairIndex) == 0xFFFF &&
                   dec.memory.reads == std::vector<uint16_t>{0x0001},
               "DEC rr fetches the next opcode after the IDU cycle");
    }

    struct AddCase {
        uint8_t opcode;
        unsigned sourcePair;
        uint16_t initialHL;
        uint16_t source;
        uint16_t afterLowByte;
        uint16_t result;
        bool lowHalfCarry;
        bool lowCarry;
        bool finalHalfCarry;
        bool finalCarry;
    };
    const AddCase addCases[] = {
        {0x09, 0, 0x0101, 0x0201, 0x0102, 0x0302, false, false, false, false},
        {0x09, 0, 0x0FFF, 0x0001, 0x0F00, 0x1000, true, true, true, false},
        {0x19, 1, 0x12FF, 0x00F0, 0x12EF, 0x13EF, false, true, false, false},
        {0x19, 1, 0x80FE, 0x0102, 0x8000, 0x8200, true, true, false, false},
        {0x29, 2, 0x7FFF, 0x0000, 0x7FFE, 0xFFFE, true, true, true, false},
        {0x39, 3, 0xFFFF, 0x0001, 0xFF00, 0x0000, true, true, true, true},
    };
    for (const auto &test : addCases) {
        CpuFixture add{{0x0000, test.opcode}, {0x0001, 0x00}};
        Reg reg = add.cpu.getReg();
        reg.getRefHL() = test.initialHL;
        if (test.sourcePair != 2)
            commonPairRef(reg, test.sourcePair) = test.source;
        setFlags(reg, true, true, false, false);
        add.cpu.setReg(reg);

        const auto lowByte = add.cpu.stepOneCycle();
        reg = add.cpu.getReg();
        const GB_Flag lowFlags = reg.getFlag();
        expect(lowByte.success && !lowByte.fetchedNewInst &&
                   reg.getRefHL() == test.afterLowByte && add.memory.reads.empty(),
               "ADD HL,rr updates L in its low-byte ALU M-cycle");
        expect(lowFlags.Z && !lowFlags.N && lowFlags.H == test.lowHalfCarry &&
                   lowFlags.C == test.lowCarry,
               "ADD HL,rr exposes low-byte half-carry and carry before the high-byte phase");

        const auto highByte = add.cpu.stepOneCycle();
        reg = add.cpu.getReg();
        const GB_Flag finalFlags = reg.getFlag();
        expect(highByte.success && highByte.fetchedNewInst &&
                   reg.getRefHL() == test.result &&
                   add.memory.reads == std::vector<uint16_t>{0x0001},
               "ADD HL,rr updates H and fetches after its high-byte ALU M-cycle");
        expect(finalFlags.Z && !finalFlags.N &&
                   finalFlags.H == test.finalHalfCarry && finalFlags.C == test.finalCarry,
               "ADD HL,rr keeps Z and reports final high-byte flags");
    }
}

void testPushBusOrder() {
    CpuFixture f{{0x0000, 0xC5}, {0x0001, 0x00}};
    Reg reg = f.cpu.getReg();
    reg.getRefSP() = 0xC100;
    reg.getRefBC() = 0x1234;
    f.cpu.setReg(reg);

    expect(!f.cpu.stepOneCycle().fetchedNewInst, "PUSH starts with an internal cycle");
    expect(f.cpu.getReg().getRefSP() == 0xC0FF,
           "PUSH decrements SP during its internal cycle");
    expect(f.memory.writes.empty(), "PUSH has no bus write in its first M-cycle");
    expect(!f.cpu.stepOneCycle().fetchedNewInst, "PUSH writes the high byte before low byte");
    expect(f.cpu.getReg().getRefSP() == 0xC0FE,
           "PUSH decrements SP again in the high-byte write M-cycle");
    expect(f.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{{0xC0FF, 0x12}},
           "PUSH writes high byte at SP-1");
    expect(!f.cpu.stepOneCycle().fetchedNewInst, "PUSH writes its low byte in the next M-cycle");
    expect(f.cpu.getReg().getRefSP() == 0xC0FE,
           "PUSH keeps the decremented SP during the low-byte write M-cycle");
    expect(f.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{
               {0xC0FF, 0x12}, {0xC0FE, 0x34}},
           "PUSH writes low byte at SP-2");
    expect(f.cpu.stepOneCycle().fetchedNewInst, "PUSH fetches the next opcode in cycle four");
    reg = f.cpu.getReg();
    expect(reg.getRefSP() == 0xC0FE, "PUSH decrements SP twice");
}

void testPushAFStackOrder() {
    CpuFixture f{{0x0000, 0xF5}, {0x0001, 0x00}};
    Reg reg = f.cpu.getReg();
    reg.getRefSP() = 0xC100;
    reg.getRefA() = 0xA5;
    setFlags(reg, true, false, true, true);
    f.cpu.setReg(reg);

    const auto internal = f.cpu.stepOneCycle();
    expect(internal.success && !internal.fetchedNewInst &&
               internal.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               f.cpu.getReg().getRefSP() == 0xC0FF &&
               f.memory.writes.empty(),
           "PUSH AF decrements SP in its internal cycle before writing");

    const auto highByte = f.cpu.stepOneCycle();
    expect(highByte.success && !highByte.fetchedNewInst &&
               highByte.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               f.cpu.getReg().getRefSP() == 0xC0FE &&
               f.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{{0xC0FF, 0xA5}},
           "PUSH AF writes A to the first stack address");

    const auto lowByte = f.cpu.stepOneCycle();
    expect(lowByte.success && !lowByte.fetchedNewInst &&
               lowByte.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               f.cpu.getReg().getRefSP() == 0xC0FE &&
               f.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{
                   {0xC0FF, 0xA5}, {0xC0FE, 0xB0}},
           "PUSH AF writes the packed flag byte after A without another SP decrement");

    const auto nextOpcode = f.cpu.stepOneCycle();
    reg = f.cpu.getReg();
    expect(nextOpcode.success && nextOpcode.fetchedNewInst &&
               nextOpcode.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               f.memory.reads == std::vector<uint16_t>{0x0001} &&
               f.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{
                   {0xC0FF, 0xA5}, {0xC0FE, 0xB0}} &&
               reg.getRefSP() == 0xC0FE && reg.getRefPC() == 0x0002,
           "PUSH AF fetches the following opcode after both stack writes");
}

void testPopAFMasksUnusedFlagBits() {
    CpuFixture f{{0x0000, 0xF1}, {0x0001, 0x00}, {0xC000, 0x2F}, {0xC001, 0xAB}};
    Reg reg = f.cpu.getReg();
    reg.getRefSP() = 0xC000;
    reg.getRefAF() = 0x56D0;
    f.cpu.setReg(reg);

    const auto lowByte = f.cpu.stepOneCycle();
    reg = f.cpu.getReg();
    expect(lowByte.success && !lowByte.fetchedNewInst &&
               lowByte.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               reg.getRefAF() == 0x56D0 && reg.getRefSP() == 0xC001 &&
               f.memory.reads == std::vector<uint16_t>{0xC000} &&
               f.memory.writes.empty(),
           "POP AF reads the flag byte without committing AF in cycle two");
    const GB_Flag oldFlags = reg.getFlag();
    expect(oldFlags.Z && oldFlags.N && !oldFlags.H && oldFlags.C,
           "POP AF keeps the old flags until the register-pair commit");

    const auto highByte = f.cpu.stepOneCycle();
    reg = f.cpu.getReg();
    expect(highByte.success && !highByte.fetchedNewInst &&
               highByte.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               reg.getRefAF() == 0x56D0 && reg.getRefSP() == 0xC002 &&
               f.memory.reads == std::vector<uint16_t>{0xC000, 0xC001} &&
               f.memory.writes.empty(),
           "POP AF reads the accumulator byte before committing AF in cycle three");

    const auto commit = f.cpu.stepOneCycle();
    reg = f.cpu.getReg();
    const GB_Flag flags = reg.getFlag();
    expect(commit.success && commit.fetchedNewInst &&
               commit.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               f.memory.reads == std::vector<uint16_t>{0xC000, 0xC001, 0x0001} &&
               f.memory.writes.empty() && reg.getRefAF() == 0xAB20 &&
               reg.getRefF() == 0x20 && reg.getRefSP() == 0xC002 &&
               reg.getRefPC() == 0x0002,
           "POP AF commits both bytes and masks the flag low nibble with next fetch");
    expect(!flags.Z && !flags.N && flags.H && !flags.C,
           "POP AF restores flags from the upper nibble of the popped flag byte");
}

void testPopRetAndJumpBusTiming() {
    CpuFixture pop{{0x0000, 0xC1}, {0x0001, 0x00}, {0xC000, 0x34}, {0xC001, 0x12}};
    Reg reg = pop.cpu.getReg();
    reg.getRefSP() = 0xC000;
    pop.cpu.setReg(reg);
    expect(!pop.cpu.stepOneCycle().fetchedNewInst, "POP reads its low byte in cycle one");
    expect(pop.cpu.getReg().getRefSP() == 0xC001,
           "POP increments SP in the low-byte read M-cycle");
    expect(!pop.cpu.stepOneCycle().fetchedNewInst, "POP reads its high byte in cycle two");
    expect(pop.cpu.getReg().getRefSP() == 0xC002,
           "POP increments SP again in the high-byte read M-cycle");
    expect(pop.cpu.stepOneCycle().fetchedNewInst,
           "POP commits the register pair and fetches the next opcode in cycle three");
    reg = pop.cpu.getReg();
    expect(reg.getRefBC() == 0x1234 && reg.getRefSP() == 0xC002,
           "POP reads low byte then high byte and increments SP twice");
    expect(pop.memory.reads == std::vector<uint16_t>{0xC000, 0xC001, 0x0001},
           "POP bus reads occur in stack order before opcode fetch");

    CpuFixture ret{{0x0000, 0xC9}, {0x0001, 0x00}, {0xC000, 0x34}, {0xC001, 0x12},
                   {0x1234, 0x00}};
    reg = ret.cpu.getReg();
    reg.getRefSP() = 0xC000;
    ret.cpu.setReg(reg);
    expect(ret.runUntilNextOpcode() == 4, "RET takes four M-cycles");
    reg = ret.cpu.getReg();
    expect(reg.getRefSP() == 0xC002 && reg.getRefPC() == 0x1235,
           "RET restores SP and fetches from the popped address");
    expect(ret.memory.reads == std::vector<uint16_t>{0xC000, 0xC001, 0x1234},
           "RET reads stack low then high and fetches the target after internal cycle");

    CpuFixture jp{{0x0000, 0xC3}, {0x0001, 0x34}, {0x0002, 0x12}, {0x1234, 0x00}};
    expect(jp.runUntilNextOpcode() == 4, "JP nn takes four M-cycles");
    expect(jp.memory.reads == std::vector<uint16_t>{0x0001, 0x0002, 0x1234},
           "JP reads address low then high and fetches at the target");
}

void testConditionalBranchAndCallBusTiming() {
    CpuFixture jr{{0x0000, 0x20}, {0x0001, 0x05}, {0x0002, 0x00}}; // JR NZ, +5
    Reg reg = jr.cpu.getReg();
    setFlags(reg, true, false, false, false);
    jr.cpu.setReg(reg);
    expect(jr.runUntilNextOpcode() == 2, "untaken JR cc skips its internal cycle");
    expect(jr.memory.reads == std::vector<uint16_t>{0x0001, 0x0002},
           "untaken JR reads only the displacement and next opcode");

    CpuFixture call{{0x0000, 0xC4}, {0x0001, 0x34}, {0x0002, 0x12}, {0x0003, 0x00}}; // CALL NZ
    reg = call.cpu.getReg();
    reg.getRefSP() = 0xC100;
    setFlags(reg, true, false, false, false);
    call.cpu.setReg(reg);
    expect(call.runUntilNextOpcode() == 3, "untaken CALL cc ends after decoding its target");
    expect(call.memory.writes.empty(), "untaken CALL cc does not push a return address");
    expect(call.cpu.getReg().getRefSP() == 0xC100,
           "untaken CALL cc does not decrement SP");

    CpuFixture taken{{0x0000, 0xCD}, {0x0001, 0x34}, {0x0002, 0x12}, {0x1234, 0x00}};
    reg = taken.cpu.getReg();
    reg.getRefSP() = 0xC100;
    taken.cpu.setReg(reg);
    expect(taken.runUntilNextOpcode() == 6, "CALL nn takes six M-cycles");
    expect(taken.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{
               {0xC0FF, 0x00}, {0xC0FE, 0x03}},
           "CALL pushes the return PC high byte then low byte");
    expect(taken.memory.reads == std::vector<uint16_t>{0x0001, 0x0002, 0x1234},
           "CALL reads the target then fetches from it after stack writes");
}

void testConditionalJumpConditionsAndTiming() {
    for (unsigned condition = 0; condition < 4; ++condition) {
        for (const bool taken : {false, true}) {
            bool z = false;
            bool c = false;
            switch (condition) {
                case 0: z = !taken; break; // NZ
                case 1: z = taken; break;  // Z
                case 2: c = !taken; break; // NC
                case 3: c = taken; break;  // C
            }

            const auto opcode = static_cast<uint8_t>(0xC2 + (condition << 3));
            CpuFixture jp{{0x0000, opcode}, {0x0001, 0x34}, {0x0002, 0x12},
                          {0x0003, 0x00}, {0x1234, 0x00}};
            Reg reg = jp.cpu.getReg();
            setFlags(reg, z, true, true, c);
            jp.cpu.setReg(reg);

            const auto expectedCycles = taken ? 4U : 3U;
            expect(jp.runUntilNextOpcode() == expectedCycles,
                   "JP cc takes its extra M-cycle only when the condition is true");
            reg = jp.cpu.getReg();
            const GB_Flag flags = reg.getFlag();
            const std::vector<uint16_t> expectedReads = taken
                ? std::vector<uint16_t>{0x0001, 0x0002, 0x1234}
                : std::vector<uint16_t>{0x0001, 0x0002, 0x0003};
            expect(jp.memory.reads == expectedReads && jp.memory.writes.empty() &&
                       reg.getRefPC() == (taken ? 0x1235 : 0x0004),
                   "JP cc reads both operand bytes and fetches from the selected path");
            expect(flags.Z == z && flags.N && flags.H && flags.C == c,
                   "JP cc preserves all flags on both branch paths");
        }
    }
}

void testRelativeJumpTakenAndSignedOffsetTiming() {
    CpuFixture taken{{0x0000, 0x20}, {0x0001, 0x05}, {0x0002, 0x00}, {0x0007, 0x00}};
    Reg reg = taken.cpu.getReg();
    setFlags(reg, false, true, true, true);
    taken.cpu.setReg(reg);

    const auto displacement = taken.cpu.stepOneCycle();
    reg = taken.cpu.getReg();
    GB_Flag flags = reg.getFlag();
    expect(displacement.success && !displacement.fetchedNewInst &&
               displacement.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               taken.memory.reads == std::vector<uint16_t>{0x0001} &&
               reg.getRefPC() == 0x0002 && flags.Z == false && flags.N &&
               flags.H && flags.C,
           "taken JR NZ reads its displacement without changing PC or flags");

    const auto branch = taken.cpu.stepOneCycle();
    reg = taken.cpu.getReg();
    expect(branch.success && !branch.fetchedNewInst &&
               branch.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               taken.memory.reads == std::vector<uint16_t>{0x0001} &&
               reg.getRefPC() == 0x0007,
           "taken JR NZ applies its signed offset in the branch M-cycle");

    const auto targetFetch = taken.cpu.stepOneCycle();
    reg = taken.cpu.getReg();
    flags = reg.getFlag();
    expect(targetFetch.success && targetFetch.fetchedNewInst &&
               targetFetch.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               taken.memory.reads == std::vector<uint16_t>{0x0001, 0x0007} &&
               reg.getRefPC() == 0x0008 && flags.Z == false && flags.N &&
               flags.H && flags.C,
           "taken JR NZ fetches the target opcode after its extra M-cycle");

    CpuFixture backwards{{0x0000, 0x18}, {0x0001, 0xFE}}; // JR -2
    const auto backwardOperand = backwards.cpu.stepOneCycle();
    expect(backwardOperand.success && !backwardOperand.fetchedNewInst &&
               backwards.cpu.getReg().getRefPC() == 0x0002 &&
               backwards.memory.reads == std::vector<uint16_t>{0x0001},
           "JR reads the signed displacement before applying it");
    const auto backwardBranch = backwards.cpu.stepOneCycle();
    expect(backwardBranch.success && !backwardBranch.fetchedNewInst &&
               backwards.cpu.getReg().getRefPC() == 0x0000,
           "JR sign-extends a negative displacement when updating PC");
    const auto backwardFetch = backwards.cpu.stepOneCycle();
    expect(backwardFetch.success && backwardFetch.fetchedNewInst &&
               backwards.cpu.getReg().getRefPC() == 0x0001 &&
               backwards.memory.reads == std::vector<uint16_t>{0x0001, 0x0000},
           "JR fetches from its backward target after applying the offset");
}

void testConditionalRetStackTiming() {
    CpuFixture taken{{0x0000, 0xC0}, {0x0001, 0x00},
                     {0xC000, 0x34}, {0xC001, 0x12}, {0x1234, 0x00}}; // RET NZ
    Reg reg = taken.cpu.getReg();
    reg.getRefSP() = 0xC000;
    setFlags(reg, false, false, false, false);
    taken.cpu.setReg(reg);

    expect(!taken.cpu.stepOneCycle().fetchedNewInst &&
               taken.cpu.getReg().getRefSP() == 0xC000 && taken.memory.reads.empty(),
           "taken RET cc checks its condition before reading the stack");
    expect(!taken.cpu.stepOneCycle().fetchedNewInst &&
               taken.cpu.getReg().getRefSP() == 0xC001 &&
               taken.memory.reads == std::vector<uint16_t>{0xC000},
           "taken RET cc reads the low byte and increments SP in the same M-cycle");
    expect(!taken.cpu.stepOneCycle().fetchedNewInst &&
               taken.cpu.getReg().getRefSP() == 0xC002 &&
               taken.memory.reads == std::vector<uint16_t>{0xC000, 0xC001},
           "taken RET cc reads the high byte and increments SP in the same M-cycle");
    expect(!taken.cpu.stepOneCycle().fetchedNewInst &&
               taken.cpu.getReg().getRefPC() == 0x1234,
           "taken RET cc installs the popped PC in its internal M-cycle");
    expect(taken.cpu.stepOneCycle().fetchedNewInst &&
               taken.cpu.getReg().getRefPC() == 0x1235 &&
               taken.memory.reads == std::vector<uint16_t>{0xC000, 0xC001, 0x1234},
           "taken RET cc fetches the next opcode from the return address after five M-cycles");

    CpuFixture untaken{{0x0000, 0xC0}, {0x0001, 0x00}, {0xC000, 0x34}}; // RET NZ
    reg = untaken.cpu.getReg();
    reg.getRefSP() = 0xC000;
    setFlags(reg, true, false, false, false);
    untaken.cpu.setReg(reg);
    expect(!untaken.cpu.stepOneCycle().fetchedNewInst,
           "untaken RET cc performs its initial condition cycle");
    expect(untaken.cpu.stepOneCycle().fetchedNewInst &&
               untaken.cpu.getReg().getRefSP() == 0xC000 &&
               untaken.memory.reads == std::vector<uint16_t>{0x0001},
           "untaken RET cc skips stack reads and finishes in two M-cycles");
}

void testCallAndRstStackPointerMcycleTiming() {
    CpuFixture call{{0x0000, 0xCD}, {0x0001, 0x34}, {0x0002, 0x12},
                    {0x1234, 0x00}};
    Reg reg = call.cpu.getReg();
    reg.getRefSP() = 0xC100;
    call.cpu.setReg(reg);

    expect(!call.cpu.stepOneCycle().fetchedNewInst &&
               call.cpu.getReg().getRefSP() == 0xC100,
           "CALL reads the low target byte before touching SP");
    expect(!call.cpu.stepOneCycle().fetchedNewInst &&
               call.cpu.getReg().getRefSP() == 0xC100,
           "CALL reads the high target byte before touching SP");
    expect(!call.cpu.stepOneCycle().fetchedNewInst &&
               call.cpu.getReg().getRefSP() == 0xC0FF &&
               call.memory.writes.empty(),
           "CALL decrements SP in its internal M-cycle before writing");
    expect(!call.cpu.stepOneCycle().fetchedNewInst &&
               call.cpu.getReg().getRefSP() == 0xC0FE,
           "CALL performs its second SP decrement in the high-byte write cycle");
    expect(call.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{
               {0xC0FF, 0x00}},
           "CALL writes the return-PC high byte at the first decremented address");
    expect(!call.cpu.stepOneCycle().fetchedNewInst &&
               call.cpu.getReg().getRefSP() == 0xC0FE,
           "CALL keeps SP stable during the low-byte write cycle");
    expect(call.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{
               {0xC0FF, 0x00}, {0xC0FE, 0x03}},
           "CALL writes the return-PC low byte at the second decremented address");
    expect(call.cpu.stepOneCycle().fetchedNewInst,
           "CALL fetches the next opcode after both stack writes");

    CpuFixture conditionalCall{{0x0000, 0xC4}, {0x0001, 0x34}, {0x0002, 0x12},
                               {0x1234, 0x00}}; // CALL NZ
    reg = conditionalCall.cpu.getReg();
    reg.getRefSP() = 0xC100;
    setFlags(reg, false, false, false, false);
    conditionalCall.cpu.setReg(reg);
    conditionalCall.cpu.stepOneCycle();
    conditionalCall.cpu.stepOneCycle();
    expect(!conditionalCall.cpu.stepOneCycle().fetchedNewInst &&
               conditionalCall.cpu.getReg().getRefSP() == 0xC0FF,
           "taken CALL cc decrements SP in its internal M-cycle");
    expect(conditionalCall.memory.reads == std::vector<uint16_t>{0x0001, 0x0002} &&
               conditionalCall.memory.writes.empty(),
           "taken CALL cc reads both target bytes before stack writes");
    expect(!conditionalCall.cpu.stepOneCycle().fetchedNewInst &&
               conditionalCall.cpu.getReg().getRefSP() == 0xC0FE &&
               conditionalCall.memory.writes ==
                   std::vector<std::pair<uint16_t, uint8_t>>{{0xC0FF, 0x00}},
           "taken CALL cc writes the return-PC high byte in its second SP phase");
    expect(!conditionalCall.cpu.stepOneCycle().fetchedNewInst &&
               conditionalCall.cpu.getReg().getRefSP() == 0xC0FE &&
               conditionalCall.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{
                   {0xC0FF, 0x00}, {0xC0FE, 0x03}},
           "taken CALL cc writes the return-PC low byte without another SP decrement");
    expect(conditionalCall.cpu.stepOneCycle().fetchedNewInst &&
               conditionalCall.cpu.getReg().getRefPC() == 0x1235 &&
               conditionalCall.cpu.getReg().getRefSP() == 0xC0FE &&
               conditionalCall.memory.reads == std::vector<uint16_t>{0x0001, 0x0002, 0x1234},
           "taken CALL cc fetches from its target after the two stack writes");

    CpuFixture rst{{0x0000, 0xCF}, {0x0008, 0x00}}; // RST 08
    reg = rst.cpu.getReg();
    reg.getRefSP() = 0xC100;
    rst.cpu.setReg(reg);
    expect(!rst.cpu.stepOneCycle().fetchedNewInst &&
               rst.cpu.getReg().getRefSP() == 0xC0FF &&
               rst.memory.writes.empty(),
           "RST decrements SP during its internal M-cycle before writing");
    expect(!rst.cpu.stepOneCycle().fetchedNewInst &&
               rst.cpu.getReg().getRefSP() == 0xC0FE,
           "RST performs its second SP decrement in the high-byte write cycle");
    expect(!rst.cpu.stepOneCycle().fetchedNewInst &&
               rst.cpu.getReg().getRefSP() == 0xC0FE,
           "RST keeps SP stable during the low-byte write cycle");
    expect(rst.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{
               {0xC0FF, 0x00}, {0xC0FE, 0x01}},
           "RST writes the return address in high-byte then low-byte order");
    expect(rst.cpu.stepOneCycle().fetchedNewInst &&
               rst.cpu.getReg().getRefSP() == 0xC0FE,
           "RST fetches its vector after completing the stack writes");
}

void testStopDecodeAndHaltIdle() {
    CpuFixture stop{{0x0000, 0x10}, {0x0001, 0x00}, {0x0002, 0x00}};
    expect(!stop.cpu.stepOneCycle().fetchedNewInst, "STOP consumes its padding byte");
    expect(stop.cpu.getReg().getRefPC() == 2, "STOP advances PC past both instruction bytes");
    expect(!stop.cpu.stepOneCycle().fetchedNewInst, "STOP enters stopped state without fetching ahead");
    expect(stop.cpu.getStopped(), "STOP exposes its own suspended CPU state");
    expect(!stop.cpu.getHALT(), "STOP is distinct from HALT");
    expect(stop.cpu.getReg().getRefPC() == 2, "STOP does not advance PC while suspended");
    const auto stopped = stop.cpu.stepOneCycle();
    expect(stopped.success && !stopped.fetchedNewInst, "stopped CPU does not execute instructions");
    expect(stopped.tCyclesElapsed == 0, "STOP idle does not advance CPU T-cycles");
    stop.cpu.wakeFromStop();
    const auto resumed = stop.cpu.stepOneCycle();
    expect(resumed.success && resumed.fetchedNewInst,
           "waking STOP fetches the next opcode in a CPU cycle");
    expect(resumed.tCyclesElapsed == T_CYCLES_PER_M_CYCLE,
           "the STOP wake fetch consumes one machine cycle");
    expect(stop.cpu.getReg().getRefPC() == 3, "STOP resumes from the byte after its padding byte");

    CpuFixture stopWithNonzeroPadding{{0x0000, 0x10}, {0x0001, 0xA5}, {0x0002, 0x00}};
    stopWithNonzeroPadding.cpu.stepOneCycle();
    const auto stopResult = stopWithNonzeroPadding.cpu.stepOneCycle();
    expect(stopResult.success && !stopResult.fetchedNewInst,
           "nonzero STOP padding still completes STOP");
    expect(stopWithNonzeroPadding.cpu.getStopped(),
           "STOP ignores the value of its second byte");
    expect(stopWithNonzeroPadding.cpu.getReg().getRefPC() == 2,
           "nonzero STOP padding still advances PC by two bytes");

    CpuFixture halt{{0x0000, 0x76}, {0x0001, 0x00}};
    expect(halt.cpu.stepOneCycle().fetchedNewInst, "HALT completes its opcode cycle");
    expect(halt.cpu.getHALT() && !halt.cpu.getStopped(), "HALT is distinct from STOP");
    const size_t readsBeforeHaltIdle = halt.memory.reads.size();
    const auto haltedIdle = halt.cpu.stepOneCycle();
    expect(haltedIdle.success, "HALT advances an idle machine cycle");
    expect(haltedIdle.tCyclesElapsed == T_CYCLES_PER_M_CYCLE,
           "HALT idle advances the CPU clock by one machine cycle");
    expect(halt.memory.reads.size() == readsBeforeHaltIdle, "HALT idle does not fetch instructions");
    halt.cpu.setHALT(false);
    expect(halt.cpu.stepOneCycle().fetchedNewInst, "waking HALT resumes the prefetched instruction");
}

void testMemoryAccessesOccurWithinTheirMCycle() {
    expect(T_CYCLES_PER_M_CYCLE == 4, "one CPU machine cycle contains four T-cycles");

    const unsigned targetIndices[] = {0, 1, 2, 3, 4, 5, 7};
    const uint8_t targetOldValues[] = {0x11, 0x22, 0x33, 0x44, 0xC0, 0x00, 0x77};
    const uint8_t loadOpcodes[] = {0x46, 0x4E, 0x56, 0x5E, 0x66, 0x6E, 0x7E};
    for (size_t i = 0; i < std::size(loadOpcodes); ++i) {
        CpuFixture f{{0x0000, loadOpcodes[i]}, {0x0001, 0x00}, {0xC000, 0xA5}};
        Reg reg = f.cpu.getReg();
        reg.getRefB() = 0x11;
        reg.getRefC() = 0x22;
        reg.getRefD() = 0x33;
        reg.getRefE() = 0x44;
        reg.getRefH() = 0xC0;
        reg.getRefL() = 0x00;
        reg.getRefA() = 0x77;
        setFlags(reg, true, true, true, true);
        f.cpu.setReg(reg);

        const auto dataRead = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        GB_Flag flags = reg.getFlag();
        expect(dataRead.success && !dataRead.fetchedNewInst &&
                   dataRead.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == std::vector<uint16_t>{0xC000} &&
                   f.memory.writes.empty() && reg.getRefHL() == 0xC000 &&
                   byteRegisterRef(reg, targetIndices[i]) == targetOldValues[i] &&
                   reg.getRefPC() == 1 && flags.Z && flags.N && flags.H && flags.C,
               "LD r,(HL) reads in M2 and leaves its target and flags unchanged during that phase");

        const auto nextOpcode = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        flags = reg.getFlag();
        expect(nextOpcode.success && nextOpcode.fetchedNewInst &&
                   nextOpcode.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == std::vector<uint16_t>({0xC000, 0x0001}) &&
                   f.memory.writes.empty() &&
                   byteRegisterRef(reg, targetIndices[i]) == 0xA5 &&
                   reg.getRefPC() == 2 && flags.Z && flags.N && flags.H && flags.C,
               "LD r,(HL) commits the selected register with the following opcode fetch");
    }

    const uint8_t storeOpcodes[] = {0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x77};
    const uint8_t sourceValues[] = {0x11, 0x22, 0x33, 0x44, 0xC0, 0x00, 0x77};
    for (size_t i = 0; i < std::size(storeOpcodes); ++i) {
        CpuFixture f{{0x0000, storeOpcodes[i]}, {0x0001, 0x00}, {0xC000, 0x00}};
        Reg reg = f.cpu.getReg();
        reg.getRefB() = 0x11;
        reg.getRefC() = 0x22;
        reg.getRefD() = 0x33;
        reg.getRefE() = 0x44;
        reg.getRefH() = 0xC0;
        reg.getRefL() = 0x00;
        reg.getRefA() = 0x77;
        setFlags(reg, true, true, true, true);
        f.cpu.setReg(reg);

        const auto dataWrite = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        GB_Flag flags = reg.getFlag();
        expect(dataWrite.success && !dataWrite.fetchedNewInst &&
                   dataWrite.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads.empty() &&
                   f.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{
                       {0xC000, sourceValues[i]}} &&
                   f.memory.bytes[0xC000] == sourceValues[i] && reg.getRefHL() == 0xC000 &&
                   reg.getRefPC() == 1 && flags.Z && flags.N && flags.H && flags.C,
               "LD (HL),r writes the selected register in M2 without changing flags");

        const auto nextOpcode = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        flags = reg.getFlag();
        expect(nextOpcode.success && nextOpcode.fetchedNewInst &&
                   nextOpcode.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == std::vector<uint16_t>{0x0001} &&
                   f.memory.writes.size() == 1 && reg.getRefHL() == 0xC000 &&
                   flags.Z && flags.N && flags.H && flags.C,
               "LD (HL),r fetches the next opcode after its data write");
    }

    CpuFixture immediate{{0x0000, 0x36}, {0x0001, 0xB6},
                         {0x0002, 0x00}, {0xC000, 0x00}};
    Reg reg = immediate.cpu.getReg();
    reg.getRefHL() = 0xC000;
    setFlags(reg, true, true, true, true);
    immediate.cpu.setReg(reg);

    const auto immediateRead = immediate.cpu.stepOneCycle();
    reg = immediate.cpu.getReg();
    GB_Flag flags = reg.getFlag();
    expect(immediateRead.success && !immediateRead.fetchedNewInst &&
               immediateRead.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               immediate.memory.reads == std::vector<uint16_t>{0x0001} &&
               immediate.memory.writes.empty() && immediate.memory.bytes[0xC000] == 0 &&
               reg.getRefHL() == 0xC000 && reg.getRefPC() == 2 &&
               flags.Z && flags.N && flags.H && flags.C,
           "LD (HL),n reads the immediate in M2 without writing memory or changing flags");

    const auto immediateWrite = immediate.cpu.stepOneCycle();
    expect(immediateWrite.success && !immediateWrite.fetchedNewInst &&
               immediateWrite.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               immediate.memory.reads == std::vector<uint16_t>{0x0001} &&
               immediate.memory.writes ==
                   std::vector<std::pair<uint16_t, uint8_t>>{{0xC000, 0xB6}} &&
               immediate.memory.bytes[0xC000] == 0xB6 &&
               immediate.cpu.getReg().getRefPC() == 2,
           "LD (HL),n writes the saved immediate to (HL) in M3");

    const auto followingOpcode = immediate.cpu.stepOneCycle();
    reg = immediate.cpu.getReg();
    flags = reg.getFlag();
    expect(followingOpcode.success && followingOpcode.fetchedNewInst &&
               followingOpcode.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               immediate.memory.reads == std::vector<uint16_t>({0x0001, 0x0002}) &&
               immediate.memory.writes.size() == 1 && reg.getRefHL() == 0xC000 &&
               reg.getRefPC() == 3 && flags.Z && flags.N && flags.H && flags.C,
           "LD (HL),n fetches the next opcode after its M3 write");
}

void testAbsoluteAndHighMemoryALoadStorePhases() {
    for (const bool isLoad : {true, false}) {
        const uint8_t opcode = isLoad ? 0xFA : 0xEA;
        CpuFixture f{{0x0000, opcode}, {0x0001, 0x45}, {0x0002, 0xC3},
                     {0x0003, 0x00}, {0xC345, 0xA5}};
        Reg reg = f.cpu.getReg();
        const uint8_t initialA = isLoad ? 0x31 : 0x96;
        reg.getRefA() = initialA;
        setFlags(reg, true, true, true, true);
        f.cpu.setReg(reg);

        const auto addressLow = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        GB_Flag flags = reg.getFlag();
        expect(addressLow.success && !addressLow.fetchedNewInst &&
                   addressLow.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == std::vector<uint16_t>{0x0001} &&
                   f.memory.writes.empty() && reg.getRefA() == initialA &&
                   reg.getRefPC() == 2 && flags.Z && flags.N && flags.H && flags.C,
               "absolute A load/store reads the address low byte in M2 only");

        const auto addressHigh = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        flags = reg.getFlag();
        expect(addressHigh.success && !addressHigh.fetchedNewInst &&
                   addressHigh.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == std::vector<uint16_t>({0x0001, 0x0002}) &&
                   f.memory.writes.empty() && reg.getRefA() == initialA &&
                   reg.getRefPC() == 3 && flags.Z && flags.N && flags.H && flags.C,
               "absolute A load/store reads the address high byte before data access");

        const auto dataAccess = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        flags = reg.getFlag();
        if (isLoad) {
            expect(dataAccess.success && !dataAccess.fetchedNewInst &&
                       dataAccess.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                       f.memory.reads == std::vector<uint16_t>({0x0001, 0x0002, 0xC345}) &&
                       f.memory.writes.empty() && f.memory.bytes[0xC345] == 0xA5 &&
                       reg.getRefA() == initialA && reg.getRefPC() == 3 &&
                       flags.Z && flags.N && flags.H && flags.C,
                   "LD A,(a16) reads data in M4 without committing A or flags yet");
        } else {
            expect(dataAccess.success && !dataAccess.fetchedNewInst &&
                       dataAccess.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                       f.memory.reads == std::vector<uint16_t>({0x0001, 0x0002}) &&
                       f.memory.writes ==
                           std::vector<std::pair<uint16_t, uint8_t>>{{0xC345, initialA}} &&
                       f.memory.bytes[0xC345] == initialA && reg.getRefA() == initialA &&
                       reg.getRefPC() == 3 && flags.Z && flags.N && flags.H && flags.C,
                   "LD (a16),A writes A to the resolved address in M4");
        }

        const auto nextOpcode = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        flags = reg.getFlag();
        const std::vector<uint16_t> expectedReads = isLoad
            ? std::vector<uint16_t>{0x0001, 0x0002, 0xC345, 0x0003}
            : std::vector<uint16_t>{0x0001, 0x0002, 0x0003};
        const std::vector<std::pair<uint16_t, uint8_t>> expectedWrites = isLoad
            ? std::vector<std::pair<uint16_t, uint8_t>>{}
            : std::vector<std::pair<uint16_t, uint8_t>>{{0xC345, initialA}};
        expect(nextOpcode.success && nextOpcode.fetchedNewInst &&
                   nextOpcode.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == expectedReads && f.memory.writes == expectedWrites &&
                   reg.getRefA() == (isLoad ? 0xA5 : initialA) && reg.getRefPC() == 4 &&
                   flags.Z && flags.N && flags.H && flags.C,
               "absolute A load/store completes at the next opcode fetch without flag changes");
    }

    for (const bool isLoad : {true, false}) {
        const uint8_t opcode = isLoad ? 0xF0 : 0xE0;
        CpuFixture f{{0x0000, opcode}, {0x0001, 0x80}, {0x0002, 0x00},
                     {0xFF80, 0x5A}};
        Reg reg = f.cpu.getReg();
        const uint8_t initialA = isLoad ? 0x31 : 0x96;
        reg.getRefA() = initialA;
        setFlags(reg, true, true, true, true);
        f.cpu.setReg(reg);

        const auto offsetRead = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        GB_Flag flags = reg.getFlag();
        expect(offsetRead.success && !offsetRead.fetchedNewInst &&
                   offsetRead.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == std::vector<uint16_t>{0x0001} &&
                   f.memory.writes.empty() && reg.getRefA() == initialA &&
                   reg.getRefPC() == 2 && flags.Z && flags.N && flags.H && flags.C,
               "LDH A,(n) and LDH (n),A read the offset in M2 without accessing data");

        const auto dataAccess = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        flags = reg.getFlag();
        if (isLoad) {
            expect(dataAccess.success && !dataAccess.fetchedNewInst &&
                       dataAccess.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                       f.memory.reads == std::vector<uint16_t>({0x0001, 0xFF80}) &&
                       f.memory.writes.empty() && reg.getRefA() == initialA &&
                       reg.getRefPC() == 2 && flags.Z && flags.N && flags.H && flags.C,
                   "LDH A,(n) reads from FF00+n in M3 before A is committed");
        } else {
            expect(dataAccess.success && !dataAccess.fetchedNewInst &&
                       dataAccess.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                       f.memory.reads == std::vector<uint16_t>{0x0001} &&
                       f.memory.writes ==
                           std::vector<std::pair<uint16_t, uint8_t>>{{0xFF80, initialA}} &&
                       f.memory.bytes[0xFF80] == initialA && reg.getRefA() == initialA &&
                       reg.getRefPC() == 2 && flags.Z && flags.N && flags.H && flags.C,
                   "LDH (n),A writes to FF00+n in M3");
        }

        const auto nextOpcode = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        flags = reg.getFlag();
        const std::vector<uint16_t> expectedReads = isLoad
            ? std::vector<uint16_t>{0x0001, 0xFF80, 0x0002}
            : std::vector<uint16_t>{0x0001, 0x0002};
        const std::vector<std::pair<uint16_t, uint8_t>> expectedWrites = isLoad
            ? std::vector<std::pair<uint16_t, uint8_t>>{}
            : std::vector<std::pair<uint16_t, uint8_t>>{{0xFF80, initialA}};
        expect(nextOpcode.success && nextOpcode.fetchedNewInst &&
                   nextOpcode.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == expectedReads && f.memory.writes == expectedWrites &&
                   reg.getRefA() == (isLoad ? 0x5A : initialA) && reg.getRefPC() == 3 &&
                   flags.Z && flags.N && flags.H && flags.C,
               "LDH immediate load/store completes at the next opcode fetch");
    }

    for (const bool isLoad : {true, false}) {
        const uint8_t opcode = isLoad ? 0xF2 : 0xE2;
        CpuFixture f{{0x0000, opcode}, {0x0001, 0x00}, {0xFFA3, 0x6C}};
        Reg reg = f.cpu.getReg();
        const uint8_t initialA = isLoad ? 0x31 : 0x96;
        reg.getRefA() = initialA;
        reg.getRefC() = 0xA3;
        setFlags(reg, true, true, true, true);
        f.cpu.setReg(reg);

        const auto dataAccess = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        GB_Flag flags = reg.getFlag();
        if (isLoad) {
            expect(dataAccess.success && !dataAccess.fetchedNewInst &&
                       dataAccess.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                       f.memory.reads == std::vector<uint16_t>{0xFFA3} &&
                       f.memory.writes.empty() && reg.getRefA() == initialA &&
                       reg.getRefC() == 0xA3 && reg.getRefPC() == 1 &&
                       flags.Z && flags.N && flags.H && flags.C,
                   "LDH A,(C) reads FF00+C in M2 before committing A");
        } else {
            expect(dataAccess.success && !dataAccess.fetchedNewInst &&
                       dataAccess.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                       f.memory.reads.empty() &&
                       f.memory.writes ==
                           std::vector<std::pair<uint16_t, uint8_t>>{{0xFFA3, initialA}} &&
                       f.memory.bytes[0xFFA3] == initialA && reg.getRefA() == initialA &&
                       reg.getRefC() == 0xA3 && reg.getRefPC() == 1 &&
                       flags.Z && flags.N && flags.H && flags.C,
                   "LDH (C),A writes to FF00+C in M2");
        }

        const auto nextOpcode = f.cpu.stepOneCycle();
        reg = f.cpu.getReg();
        flags = reg.getFlag();
        const std::vector<uint16_t> expectedReads = isLoad
            ? std::vector<uint16_t>{0xFFA3, 0x0001}
            : std::vector<uint16_t>{0x0001};
        const std::vector<std::pair<uint16_t, uint8_t>> expectedWrites = isLoad
            ? std::vector<std::pair<uint16_t, uint8_t>>{}
            : std::vector<std::pair<uint16_t, uint8_t>>{{0xFFA3, initialA}};
        expect(nextOpcode.success && nextOpcode.fetchedNewInst &&
                   nextOpcode.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
                   f.memory.reads == expectedReads && f.memory.writes == expectedWrites &&
                   reg.getRefA() == (isLoad ? 0x6C : initialA) && reg.getRefC() == 0xA3 &&
                   reg.getRefPC() == 2 && flags.Z && flags.N && flags.H && flags.C,
               "LDH C-register load/store completes at the next opcode fetch");
    }
}

void testEiDelay() {
    CpuFixture f{{0x0000, 0xFB}, {0x0001, 0x01}, {0x0002, 0x34},
                 {0x0003, 0x12}, {0x0004, 0x00}};
    f.cpu.setIME(false);
    expect(f.cpu.stepOneCycle().fetchedNewInst, "EI completes in one M-cycle");
    expect(!f.cpu.getIME(), "EI does not enable IME during its own instruction");
    expect(!f.cpu.stepOneCycle().fetchedNewInst, "following multi-cycle instruction starts");
    expect(!f.cpu.getIME(), "IME stays disabled while the following instruction runs");
    expect(!f.cpu.stepOneCycle().fetchedNewInst, "following instruction reads its high byte");
    expect(!f.cpu.getIME(), "IME stays disabled until the following instruction completes");
    expect(f.cpu.stepOneCycle().fetchedNewInst, "following instruction completes");
    expect(f.cpu.getIME(), "EI enables IME after the following instruction");

    CpuFixture eiDi{{0x0000, 0xFB}, {0x0001, 0xF3}, {0x0002, 0x00}};
    eiDi.cpu.setIME(false);
    eiDi.cpu.stepOneCycle();
    eiDi.cpu.stepOneCycle();
    expect(!eiDi.cpu.getIME(), "DI cancels a pending EI enable");

    CpuFixture eiPending{{0x0000, 0xFB}, {0x0001, 0x06}, {0x0002, 0xA5},
                         {0x0003, 0x00}, {0x0040, 0x00}};
    eiPending.cpu.setIME(false);
    eiPending.interrupts.write(0xFFFF, 0x01);
    eiPending.interrupts.write(0xFF0F, 0x01);
    eiPending.cpu.stepOneCycle();
    expect(!eiPending.cpu.getIME(), "EI keeps IME disabled through its own cycle");
    expect(!eiPending.cpu.stepOneCycle().fetchedNewInst,
           "the instruction after EI starts before a pending interrupt is accepted");
    expect(eiPending.cpu.stepOneCycle().fetchedNewInst,
           "the instruction after EI completes before interrupt entry");
    expect(eiPending.cpu.getIME(), "IME becomes enabled at the following instruction boundary");
    expect(eiPending.cpu.stepOneCycle().success && !eiPending.cpu.getIME(),
           "the pending interrupt is accepted at the next instruction boundary");
}

void testRepeatedEiDoesNotPostponePendingInterrupt() {
    CpuFixture f{{0x0000, 0xFB}, {0x0001, 0xFB}, {0x0002, 0x04},
                 {0x0040, 0x00}};
    auto reg = f.cpu.getReg();
    reg.getRefSP() = 0xC100;
    reg.getRefB() = 0;
    f.cpu.setReg(reg);
    f.cpu.setIME(false);
    f.interrupts.write(0xFFFF, 1);
    f.interrupts.write(0xFF0F, 1);
    expect(f.runUntilNextOpcode() == 1 && !f.cpu.getIME(),
           "first EI waits for the following instruction");
    expect(f.runUntilNextOpcode() == 1 && f.cpu.getIME(),
           "second EI completes the first EI delay instead of postponing it");
    expect(f.runUntilNextOpcode() == 5, "pending interrupt enters after the second EI");
    reg = f.cpu.getReg();
    expect(reg.getRefB() == 0 && reg.getRefPC() == 0x41,
           "pending interrupt runs before the instruction following EI; EI");
    expect(f.memory.bytes[0xC0FF] == 0 && f.memory.bytes[0xC0FE] == 2,
           "EI; EI interrupt return address points after the second EI");
}

void testDiCancelsEiWithPendingInterruptAcrossFollowingInstructions() {
    CpuFixture f{{0x0000, 0xFB}, {0x0001, 0xF3}, {0x0002, 0x00},
                 {0x0003, 0x00}, {0x0004, 0x00}};
    f.cpu.setIME(false);
    f.interrupts.write(0xFFFF, 1);
    f.interrupts.write(0xFF0F, 1);
    for (unsigned instruction = 0; instruction < 4; ++instruction) {
        expect(f.runUntilNextOpcode() == 1, "EI; DI sequence continues without handler entry");
        expect(!f.cpu.getIME(), "DI cancellation persists across following instructions");
    }
    expect(f.memory.writes.empty() && (f.interrupts.read(0xFF0F) & 1),
           "cancelled EI neither pushes a return address nor acknowledges pending IF");
}

void testRetiImmediatelyServicesAnotherPendingInterrupt() {
    CpuFixture f{{0x0000, 0xD9}, {0x1234, 0x04}, {0x0050, 0x00}};
    auto reg = f.cpu.getReg();
    reg.getRefSP() = 0xC100;
    reg.getRefB() = 0;
    f.cpu.setReg(reg);
    f.memory.bytes[0xC100] = 0x34;
    f.memory.bytes[0xC101] = 0x12;
    f.cpu.setIME(false);
    f.interrupts.write(0xFFFF, 4);
    f.interrupts.write(0xFF0F, 4);
    expect(f.runUntilNextOpcode() == 4 && f.cpu.getIME(),
           "RETI restores PC and enables IME without EI's extra-instruction delay");
    expect(f.cpu.getReg().getRefSP() == 0xC102,
           "RETI pops the previous handler return address");
    expect(f.runUntilNextOpcode() == 5, "pending interrupt is accepted immediately after RETI");
    reg = f.cpu.getReg();
    expect(reg.getRefB() == 0 && reg.getRefPC() == 0x51 && !f.cpu.getIME(),
           "new handler starts before executing the resumed INC B instruction");
    expect(f.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{
               {0xC101, 0x12}, {0xC100, 0x34}},
           "new interrupt pushes the unexecuted resumed opcode's address");
    expect((f.interrupts.read(0xFF0F) & 4) == 0,
           "new service acknowledges the pending timer interrupt");
}

void testInterruptEntryPriorityStackAndVector() {
    CpuFixture f{{0x0000, 0x00}, {0x0001, 0x00}, {0x0040, 0x00},
                 {0x0048, 0x00}, {0x0050, 0x00}};
    Reg reg = f.cpu.getReg();
    reg.getRefSP() = 0xC100;
    f.cpu.setReg(reg);
    expect(f.cpu.stepOneCycle().fetchedNewInst,
           "the instruction before interrupt completes and prefetches next opcode");

    f.interrupts.write(0xFFFF, 0xFF);
    f.interrupts.write(0xFF0F, 0x85); // VBlank and timer requests; VBlank wins.
    const size_t readsBeforeInterrupt = f.memory.reads.size();
    const auto cycle0 = f.cpu.stepOneCycle();
    expect(cycle0.success && cycle0.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               !cycle0.fetchedNewInst && f.cpu.getReg().getRefSP() == 0xC100 &&
               f.memory.writes.empty(),
           "interrupt entry rewinds PC in its first internal M-cycle without touching SP");
    const auto cycle1 = f.cpu.stepOneCycle();
    expect(cycle1.success && cycle1.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               !cycle1.fetchedNewInst && f.cpu.getReg().getRefSP() == 0xC0FF &&
               f.memory.writes.empty(),
           "interrupt entry performs its first implied SP decrement before stack writes");
    const auto cycle2 = f.cpu.stepOneCycle();
    expect(cycle2.success && cycle2.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               !cycle2.fetchedNewInst && f.cpu.getReg().getRefSP() == 0xC0FE &&
               f.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{{0xC0FF, 0x00}},
           "interrupt entry writes PC high and performs its second SP decrement together");
    const auto cycle3 = f.cpu.stepOneCycle();
    expect(cycle3.success && cycle3.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               !cycle3.fetchedNewInst && f.cpu.getReg().getRefSP() == 0xC0FE &&
               f.cpu.getReg().getRefPC() == 0x0040 &&
               f.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{
                   {0xC0FF, 0x00}, {0xC0FE, 0x01}},
           "interrupt entry writes PC low and installs the vector in the same M-cycle");
    const auto cycle4 = f.cpu.stepOneCycle();
    expect(cycle4.success && cycle4.tCyclesElapsed == T_CYCLES_PER_M_CYCLE &&
               cycle4.fetchedNewInst,
           "interrupt entry fetches the vector opcode in its fifth cycle");

    reg = f.cpu.getReg();
    expect(!f.cpu.getIME(), "interrupt entry disables IME");
    expect(reg.getRefPC() == 0x0041, "interrupt entry jumps to the highest-priority vector");
    expect(reg.getRefSP() == 0xC0FE, "interrupt entry pushes two bytes onto the stack");
    expect(f.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{
               {0xC0FF, 0x00}, {0xC0FE, 0x01}},
           "interrupt entry pushes return PC high byte then low byte");
    expect(f.interrupts.read(0xFF0F) == 0xE4,
           "interrupt entry acknowledges only the selected VBlank request");
    expect(f.memory.reads.size() == readsBeforeInterrupt + 1 &&
               f.memory.reads.back() == 0x0040,
           "interrupt entry fetches the handler opcode after its stack writes");
}

void testInterruptWaitsUntilCbInstructionCompletes() {
    CpuFixture f{{0x0000, 0xCB}, {0x0001, 0x33}, {0x0002, 0x00},
                 {0x0040, 0x00}};
    Reg reg = f.cpu.getReg();
    reg.getRefSP() = 0xC100;
    reg.getRefE() = 0xF0;
    f.cpu.setReg(reg);

    const auto prefixCycle = f.cpu.stepOneCycle();
    expect(prefixCycle.success && !prefixCycle.fetchedNewInst,
           "CB prefix fetch does not complete the two-byte instruction");
    expect(f.cpu.getReg().getRefPC() == 2,
           "CB prefix fetches its opcode byte before checking the next boundary");

    f.interrupts.write(0xFFFF, 0x01);
    f.interrupts.write(0xFF0F, 0x01);
    const auto cbOpcodeCycle = f.cpu.stepOneCycle();
    expect(cbOpcodeCycle.success && cbOpcodeCycle.fetchedNewInst,
           "pending interrupt does not split a CB-prefixed instruction");
    expect(f.cpu.getIME() && f.cpu.getReg().getRefE() == 0x0F,
           "CB opcode executes before the pending interrupt is accepted");

    const auto interruptStart = f.cpu.stepOneCycle();
    expect(interruptStart.success && !interruptStart.fetchedNewInst &&
               !f.cpu.getIME(),
           "pending interrupt is accepted after the CB instruction completes");
    for (unsigned cycle = 1; cycle < 5; ++cycle) {
        const auto result = f.cpu.stepOneCycle();
        expect(result.success && result.fetchedNewInst == (cycle == 4),
               "CB-following interrupt completes its five-M-cycle entry");
    }

    expect(f.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{
               {0xC0FF, 0x00}, {0xC0FE, 0x02}},
           "interrupt returns to the opcode after the completed CB instruction");
    expect(f.cpu.getReg().getRefPC() == 0x0041,
           "CB-following interrupt vectors to the requested handler");
}

void testHaltWakeAndHaltBug() {
    CpuFixture halted{{0x0000, 0x76}, {0x0001, 0x00}, {0x0002, 0x00}};
    halted.cpu.setIME(false);
    halted.cpu.stepOneCycle();
    expect(halted.cpu.getHALT(), "HALT sleeps when no enabled interrupt is pending");
    halted.interrupts.write(0xFFFF, 0x01);
    halted.interrupts.write(0xFF0F, 0x01);
    const auto wake = halted.cpu.stepOneCycle();
    expect(wake.fetchedNewInst && !halted.cpu.getHALT(),
           "a pending enabled interrupt wakes HALT even while IME is clear");
    expect(halted.cpu.getReg().getRefPC() == 3,
           "IME-clear HALT wake executes the prefetched next opcode without rewinding PC");

    CpuFixture bug{{0x0000, 0x76}, {0x0001, 0x3E}, {0x0002, 0x42}, {0x0003, 0x00}};
    bug.cpu.setIME(false);
    bug.interrupts.write(0xFFFF, 0x01);
    bug.interrupts.write(0xFF0F, 0x01);
    bug.cpu.stepOneCycle();
    expect(!bug.cpu.getHALT(), "HALT does not enter sleep when IME is clear with pending IF&IE");
    expect(bug.cpu.getReg().getRefPC() == 1,
           "HALT bug suppresses the increment of the next opcode fetch");
    expect(!bug.cpu.stepOneCycle().fetchedNewInst,
           "the following instruction reads its immediate in the next M-cycle");
    expect(bug.cpu.stepOneCycle().fetchedNewInst,
           "the HALT-bugged immediate instruction completes in its second M-cycle");
    expect(bug.cpu.getReg().getRefA() == 0x3E,
           "HALT bug makes the opcode byte repeat as the immediate operand");
}

} // namespace

int main() {
    testCbRegisterOperationsCompleteAfterPrefixCycle();
    testBitHlUsesReadAndNextFetchCycles();
    testIncDecHlReadModifyWritePhases();
    testRegisterIncDecCompletesInOneCycle();
    testRegisterToRegisterLoadsCompleteInOneCycle();
    testSingleCycleFlagAndAccumulatorOperations();
    testCbHlReadModifyWritePhases();
    testHlAluReadsBeforeUpdatingAccumulatorAndFlags();
    testImmediateAluReadsBeforeUpdatingAccumulatorAndFlags();
    testRegisterAluCompletesInOneCycle();
    testIndirectLoadsReadOldAddressAndUpdateAtCorrectPhase();
    testIndirectStoresWriteOldAddressAndUpdateAtCorrectPhase();
    testDaaBoundaryCases();
    testSpOffsetFlagsAndCycles();
    testLdSpFromHlMcycle();
    testJpHlFetchesAtTargetInSameMcycle();
    testImmediate8BitLoadPhases();
    testImmediate16BitLoadPhases();
    testSixteenBitArithmeticMcyclePhases();
    testPushBusOrder();
    testPushAFStackOrder();
    testPopAFMasksUnusedFlagBits();
    testPopRetAndJumpBusTiming();
    testConditionalBranchAndCallBusTiming();
    testConditionalJumpConditionsAndTiming();
    testRelativeJumpTakenAndSignedOffsetTiming();
    testConditionalRetStackTiming();
    testCallAndRstStackPointerMcycleTiming();
    testStopDecodeAndHaltIdle();
    testMemoryAccessesOccurWithinTheirMCycle();
    testAbsoluteAndHighMemoryALoadStorePhases();
    testEiDelay();
    testRepeatedEiDoesNotPostponePendingInterrupt();
    testDiCancelsEiWithPendingInterruptAcrossFollowingInstructions();
    testRetiImmediatelyServicesAnotherPendingInterrupt();
    testInterruptEntryPriorityStackAndVector();
    testInterruptWaitsUntilCbInstructionCompletes();
    testHaltWakeAndHaltBug();
    std::cout << "CPU instruction tests passed\n";
    return EXIT_SUCCESS;
}
