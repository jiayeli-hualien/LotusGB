#include <array>
#include <cstdlib>
#include <iostream>
#include <tuple>
#include <utility>
#include <vector>

#include "GBCPU.hpp"
#include "decoder.hpp"

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
    Decoder baseDecoder{false};
    Decoder cbDecoder{true};
    GBCPU cpu{memory, baseDecoder, cbDecoder};

    explicit CpuFixture(std::initializer_list<std::pair<uint16_t, uint8_t>> program) {
        for (const auto &[addr, byte] : program)
            memory.bytes[addr] = byte;
        cpu.reset();
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

void testCbShiftZeroFlags() {
    for (const auto &[opcode, carryIn, label] : {
             std::tuple<uint8_t, bool, const char *>(0x20, false, "SLA B"),
             std::tuple<uint8_t, bool, const char *>(0x10, false, "RL B")}) {
        CpuFixture f{{0x0000, 0xCB}, {0x0001, opcode}, {0x0002, 0x00}};
        Reg reg = f.cpu.getReg();
        reg.getRefB() = 0x80;
        setFlags(reg, false, false, false, carryIn);
        f.cpu.setReg(reg);

        f.runUntilNextOpcode();
        reg = f.cpu.getReg();
        const GB_Flag flags = reg.getFlag();
        expect(reg.getRefB() == 0, "CB shift writes the low 8-bit result");
        expect(flags.Z && flags.C, "CB shift sets Z and C from the 8-bit result");
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

    CpuFixture f{{0x0000, 0xF8}, {0x0001, 0x01}, {0x0002, 0x00}};
    Reg reg = f.cpu.getReg();
    reg.getRefSP() = 0x00FF;
    setFlags(reg, true, true, false, false);
    f.cpu.setReg(reg);
    expect(f.runUntilNextOpcode() == 3, "LD HL,SP+e8 takes three M-cycles");
    reg = f.cpu.getReg();
    expect(reg.getRefHL() == 0x0100, "LD HL,SP+e8 stores the sum in HL");
    const GB_Flag flags = reg.getFlag();
    expect(!flags.Z && !flags.N && flags.H && flags.C,
           "LD HL,SP+e8 shares the unsigned-offset H/C rules");

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
            expect(exhaustive.runUntilNextOpcode(4) == 4,
                   "ADD SP,e8 keeps its four M-cycle duration");

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

void testPushBusOrder() {
    CpuFixture f{{0x0000, 0xC5}, {0x0001, 0x00}};
    Reg reg = f.cpu.getReg();
    reg.getRefSP() = 0xC100;
    reg.getRefBC() = 0x1234;
    f.cpu.setReg(reg);

    expect(!f.cpu.stepOneCycle().fetchedNewInst, "PUSH starts with an internal cycle");
    expect(f.memory.writes.empty(), "PUSH has no bus write in its first M-cycle");
    expect(!f.cpu.stepOneCycle().fetchedNewInst, "PUSH writes the high byte before low byte");
    expect(f.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{{0xC0FF, 0x12}},
           "PUSH writes high byte at SP-1");
    expect(!f.cpu.stepOneCycle().fetchedNewInst, "PUSH writes its low byte in the next M-cycle");
    expect(f.memory.writes == std::vector<std::pair<uint16_t, uint8_t>>{
               {0xC0FF, 0x12}, {0xC0FE, 0x34}},
           "PUSH writes low byte at SP-2");
    expect(f.cpu.stepOneCycle().fetchedNewInst, "PUSH fetches the next opcode in cycle four");
    reg = f.cpu.getReg();
    expect(reg.getRefSP() == 0xC0FE, "PUSH decrements SP twice");
}

void testPopRetAndJumpBusTiming() {
    CpuFixture pop{{0x0000, 0xC1}, {0x0001, 0x00}, {0xC000, 0x34}, {0xC001, 0x12}};
    Reg reg = pop.cpu.getReg();
    reg.getRefSP() = 0xC000;
    pop.cpu.setReg(reg);
    expect(pop.runUntilNextOpcode() == 3, "POP rr takes three M-cycles");
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

void testStopDecodeAndHaltIdle() {
    CpuFixture stop{{0x0000, 0x10}, {0x0001, 0x00}, {0x0002, 0x00}};
    expect(!stop.cpu.stepOneCycle().fetchedNewInst, "STOP reads its padding byte");
    expect(!stop.cpu.stepOneCycle().fetchedNewInst, "STOP enters stopped state without fetching ahead");
    expect(stop.cpu.getStopped(), "STOP exposes its own suspended CPU state");
    expect(!stop.cpu.getHALT(), "STOP is distinct from HALT");
    const size_t readsBeforeStopIdle = stop.memory.reads.size();
    const auto stopped = stop.cpu.stepOneCycle();
    expect(stopped.success && !stopped.fetchedNewInst, "stopped CPU does not execute instructions");
    expect(stop.memory.reads.size() == readsBeforeStopIdle, "stopped CPU performs no bus reads");
    stop.cpu.wakeFromStop();
    expect(stop.memory.reads.size() == readsBeforeStopIdle,
           "waking STOP does not read the bus outside a CPU cycle");
    const auto resumed = stop.cpu.stepOneCycle();
    expect(resumed.success && resumed.fetchedNewInst,
           "waking STOP fetches the next opcode in a CPU cycle");
    expect(stop.memory.reads.back() == 0x0002, "STOP resumes from the byte after its padding byte");

    CpuFixture stopWithNonzeroPadding{{0x0000, 0x10}, {0x0001, 0xA5}, {0x0002, 0x00}};
    stopWithNonzeroPadding.cpu.stepOneCycle();
    expect(stopWithNonzeroPadding.memory.reads == std::vector<uint16_t>{0x0001},
           "STOP consumes its second byte from the bus");
    const auto stopResult = stopWithNonzeroPadding.cpu.stepOneCycle();
    expect(stopResult.success && !stopResult.fetchedNewInst,
           "nonzero STOP padding still completes STOP");
    expect(stopWithNonzeroPadding.cpu.getStopped(),
           "STOP ignores the value of its second byte");
    expect(stopWithNonzeroPadding.memory.reads == std::vector<uint16_t>{0x0001},
           "STOP does not fetch past the second byte");

    CpuFixture halt{{0x0000, 0x76}, {0x0001, 0x00}};
    expect(halt.cpu.stepOneCycle().fetchedNewInst, "HALT completes its opcode cycle");
    expect(halt.cpu.getHALT() && !halt.cpu.getStopped(), "HALT is distinct from STOP");
    const size_t readsBeforeHaltIdle = halt.memory.reads.size();
    expect(halt.cpu.stepOneCycle().success, "HALT advances an idle machine cycle");
    expect(halt.memory.reads.size() == readsBeforeHaltIdle, "HALT idle does not fetch instructions");
    halt.cpu.setHALT(false);
    expect(halt.cpu.stepOneCycle().fetchedNewInst, "waking HALT resumes the prefetched instruction");
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
}

} // namespace

int main() {
    testCbShiftZeroFlags();
    testDaaBoundaryCases();
    testSpOffsetFlagsAndCycles();
    testPushBusOrder();
    testPopRetAndJumpBusTiming();
    testConditionalBranchAndCallBusTiming();
    testStopDecodeAndHaltIdle();
    testEiDelay();
    std::cout << "CPU instruction tests passed\n";
    return EXIT_SUCCESS;
}
