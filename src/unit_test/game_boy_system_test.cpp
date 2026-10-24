#include <cstdlib>
#include <iostream>
#include <vector>

#include "game_boy_system.hpp"

namespace {

using namespace LOTUSGB;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void testCpuFetchesAndExecutesFromCartridgeThroughSystemBus() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    rom[0] = 0x3E; // LD A, $A5
    rom[1] = 0xA5;
    rom[2] = 0x00; // NOP

    GameBoySystem system = GameBoySystem::createForTest(std::move(rom));

    const auto initialFetch = system.stepOneCycle();
    expect(initialFetch.success && initialFetch.fetchedNewInst,
           "system's first cycle fetches the cartridge opcode");
    expect(initialFetch.tCyclesElapsed == T_CYCLES_PER_M_CYCLE,
           "initial cartridge fetch advances system time by one M-cycle");
    expect(system.getCpuRegisters().getRefPC() == 1,
           "cartridge opcode fetch advances CPU PC");

    const auto immediateRead = system.stepOneCycle();
    expect(immediateRead.success && !immediateRead.fetchedNewInst,
           "CPU reads the immediate byte in the next M-cycle");
    const auto instructionComplete = system.stepOneCycle();
    expect(instructionComplete.success && instructionComplete.fetchedNewInst,
           "CPU completes the instruction and fetches the next cartridge opcode");
    expect(system.getCpuRegisters().getRefA() == 0xA5,
           "CPU executes data fetched through the system bus");
    expect(system.getCpuRegisters().getRefPC() == 3,
           "CPU advances through immediate and opcode bytes");
    expect(system.getElapsedTCycles() == 3 * T_CYCLES_PER_M_CYCLE,
           "system clock accumulates all CPU M-cycles");
}

void testEchoRamReadsAndWritesWorkRam() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x3E, 0xA5,       // LD A, $A5
        0xEA, 0x00, 0xC0, // LD ($C000), A
        0x3E, 0x00,       // LD A, $00
        0xFA, 0x00, 0xE0, // LD A, ($E000)
        0x3E, 0x5A,       // LD A, $5A
        0xEA, 0x01, 0xE0, // LD ($E001), A
        0x3E, 0x00,       // LD A, $00
        0xFA, 0x01, 0xC0, // LD A, ($C001)
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[i] = program[i];

    GameBoySystem system = GameBoySystem::createForTest(std::move(rom));
    for (int cycle = 0; cycle < 25; ++cycle)
        expect(system.stepOneCycle().success,
               "echo RAM program advances through system CPU cycles");

    expect(system.getCpuRegisters().getRefA() == 0x5A,
           "system bus aliases echo RAM reads and writes to work RAM");
}

void testTimerInterruptWakesHaltAndRunsHandler() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x31, 0x00, 0xC1, // LD SP, $C100
        0x3E, 0x04,       // LD A, $04
        0xEA, 0xFF, 0xFF, // LD ($FFFF), A (enable timer interrupt)
        0x3E, 0x00,       // LD A, $00
        0xE0, 0x06,       // LDH ($FF06), A (TMA)
        0x3E, 0xFF,       // LD A, $FF
        0xE0, 0x05,       // LDH ($FF05), A (TIMA)
        0x3E, 0x05,       // LD A, $05
        0xE0, 0x07,       // LDH ($FF07), A (enable timer, 16 T-cycle frequency)
        0xFB,             // EI
        0x76,             // HALT
        0x00,             // NOP
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[i] = program[i];
    rom[0x0050] = 0x06; // LD B, $42
    rom[0x0051] = 0x42;
    rom[0x0052] = 0xD9; // RETI

    GameBoySystem system = GameBoySystem::createForTest(std::move(rom));
    bool handlerRan = false;
    for (unsigned step = 0; step < 100; ++step) {
        const auto result = system.stepOneCycle();
        expect(result.success, "timer interrupt integration step succeeds");
        if (system.getCpuRegisters().getRefB() == 0x42) {
            handlerRan = true;
            break;
        }
    }

    expect(handlerRan,
           "timer overflow requests an interrupt that wakes HALT and runs the timer handler");
}

void testSystemTimerClocksApuFrameSequencer() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x3E, 0x80,       // LD A, $80
        0xE0, 0x26,       // LDH ($FF26), A (power on APU)
        0x3E, 0x3F,       // LD A, $3F
        0xE0, 0x20,       // LDH ($FF20), A (channel 4 length = 1)
        0x3E, 0xF0,       // LD A, $F0
        0xE0, 0x21,       // LDH ($FF21), A (enable channel 4 DAC)
        0x3E, 0x00,       // LD A, $00
        0xE0, 0x22,       // LDH ($FF22), A
        0x3E, 0xC0,       // LD A, $C0
        0xE0, 0x23,       // LDH ($FF23), A (trigger, enable length)
        0xF0, 0x26,       // loop: LDH A, ($FF26)
        0x47,             // LD B, A
        0x18, 0xFB,       // JR loop
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[i] = program[i];

    GameBoySystem system = GameBoySystem::createForTest(std::move(rom));
    bool channelStarted = false;
    bool channelStopped = false;
    for (unsigned step = 0; step < 4000; ++step) {
        expect(system.stepOneCycle().success,
               "DIV-APU system integration step succeeds");
        const bool active = (system.getCpuRegisters().getRefB() & 0x08) != 0;
        channelStarted |= active;
        if (channelStarted && !active) {
            channelStopped = true;
            break;
        }
    }

    expect(channelStarted,
           "system bus writes successfully start channel 4 before its first DIV-APU edge");
    expect(channelStopped,
           "Timer DIV falling edge reaches the APU and clocks channel length");
}

void testSystemBusMapsWaveRamToApu() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x3E, 0x80,       // LD A, $80
        0xE0, 0x26,       // LDH ($FF26), A (power on APU)
        0x3E, 0xF0,       // LD A, $F0
        0xE0, 0x30,       // LDH ($FF30), A (write wave RAM)
        0x3E, 0x80,       // LD A, $80
        0xE0, 0x1A,       // LDH ($FF1A), A (enable channel 3 DAC)
        0x3E, 0x20,       // LD A, $20
        0xE0, 0x1C,       // LDH ($FF1C), A (full wave output level)
        0x3E, 0xFE,       // LD A, $FE
        0xE0, 0x1D,       // LDH ($FF1D), A
        0x3E, 0x87,       // LD A, $87
        0xE0, 0x1E,       // LDH ($FF1E), A (trigger channel 3)
        0x3E, 0x70,       // LD A, $70
        0xE0, 0x24,       // LDH ($FF24), A (left volume)
        0x3E, 0x40,       // LD A, $40
        0xE0, 0x25,       // LDH ($FF25), A (route channel 3 left)
        0x18, 0xFE,       // JR $ (keep the system running)
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[i] = program[i];

    GameBoySystem system = GameBoySystem::createForTest(std::move(rom));
    const RetStepFrame frame = system.stepFrame();
    expect(frame.success && frame.frameCompleted,
           "channel 3 system-bus integration frame completes");
    const auto samples = system.takeAudioSamples();

    bool hasLeftAudio = false;
    bool changesOverTime = false;
    int16_t previousLeft = samples.empty() ? 0 : samples[0];
    for (size_t i = 0; i < samples.size(); i += 2) {
        hasLeftAudio |= samples[i] != 0;
        changesOverTime |= samples[i] != previousLeft;
        previousLeft = samples[i];
        expect(samples[i + 1] == 0,
               "system-routed wave channel does not leak into the right output");
    }
    expect(hasLeftAudio && changesOverTime,
           "CPU writes to mapped wave RAM produce changing channel 3 PCM");
}

void testGameBoySystemAcceptsButtonInput() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x3E, 0x20,       // LD A, $20
        0xE0, 0x00,       // LDH ($FF00), A (select directions)
        0xFA, 0x00, 0xFF, // LD A, ($FF00)
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[i] = program[i];

    GameBoySystem system = GameBoySystem::createForTest(std::move(rom));
    system.setButtonState(JoypadButton::Right, true);
    for (unsigned step = 0; step < 16; ++step)
        expect(system.stepOneCycle().success, "button input program advances successfully");

    expect(system.getCpuRegisters().getRefA() == 0xEE,
           "button state reaches the CPU through the joypad register on the system bus");
}

void testPpuAdvancesAndExposesScanlineThroughSystemBus() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x3E, 0x80,       // LD A, $80
        0xE0, 0x40,       // LDH ($FF40), A (enable LCD)
        0xF0, 0x44,       // loop: LDH A, ($FF44)
        0xFE, 0x01,       // CP $01
        0x20, 0xFA,       // JR NZ, loop
        0x06, 0x5A,       // LD B, $5A (exit marker)
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[i] = program[i];

    GameBoySystem system = GameBoySystem::createForTest(std::move(rom));
    bool reachedNextLine = false;
    for (unsigned step = 0; step < 1000; ++step) {
        const auto result = system.stepOneCycle();
        expect(result.success, "PPU system integration step succeeds");
        if (system.getCpuRegisters().getRefB() == 0x5A) {
            reachedNextLine = true;
            break;
        }
    }

    expect(reachedNextLine,
           "CPU observes LY advance through the address bus as the system clock advances PPU");
    expect(system.getElapsedTCycles() >= 456,
           "system clock advances at least one complete scanline");
}

void testGameBoySystemExposesRenderedFrameBuffer() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x3E, 0x80,       // LD A, $80
        0xEA, 0x00, 0x80, // LD ($8000), A (tile 0, row 0, color 1 at x=0)
        0x3E, 0x00,       // LD A, $00
        0xEA, 0x00, 0x98, // LD ($9800), A (BG map points to tile 0)
        0x3E, 0xE4,       // LD A, $E4 (identity DMG palette)
        0xE0, 0x47,       // LDH ($FF47), A (BGP)
        0x3E, 0x91,       // LD A, $91 (LCD, unsigned tiles, BG enabled)
        0xE0, 0x40,       // LDH ($FF40), A (enable LCD)
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[i] = program[i];

    GameBoySystem system = GameBoySystem::createForTest(std::move(rom));
    for (unsigned step = 0; step < 180; ++step)
        expect(system.stepOneCycle().success, "framebuffer integration step succeeds");

    const auto &frame = system.getFrameBuffer();
    expect(frame.size() == DmgPpu::FRAME_WIDTH * DmgPpu::FRAME_HEIGHT,
           "system exposes a complete 160 by 144 framebuffer");
    expect(frame[0] == 1, "system framebuffer includes the rendered BG pixel");
}

void testDmgPostBootStartupAndFrameStepping() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0xF0, 0x04, 0x47, // LDH A, ($04); LD B, A (DIV)
        0xF0, 0x47, 0x4F, // LDH A, ($47); LD C, A (BGP)
        0xF0, 0x00, 0x57, // LDH A, ($00); LD D, A (P1)
        0xF0, 0x0F, 0x5F, // LDH A, ($0F); LD E, A (IF)
        0xF0, 0x40, 0x67, // LDH A, ($40); LD H, A (LCDC)
        0xFA, 0xFF, 0xFF, 0x6F, // LD A, ($FFFF); LD L, A (IE)
        0x18, 0xFE, // idle loop
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[0x0100 + i] = program[i];
    rom[0x014D] = 0x01; // non-zero header checksum: H/C set by DMG boot ROM

    GameBoySystem system(std::move(rom));
    Reg registers = system.getCpuRegisters();
    expect(registers.getRefPC() == 0x0100,
           "normal system startup begins at the cartridge entry point");
    expect(registers.getRefSP() == 0xFFFE,
           "DMG post-boot stack pointer is initialized");
    expect(registers.getRefAF() == 0x01B0,
           "DMG post-boot accumulator and flags match a non-zero header checksum");
    expect(registers.getRefBC() == 0x0013 && registers.getRefDE() == 0x00D8 &&
               registers.getRefHL() == 0x014D,
           "DMG post-boot general register pairs are initialized");

    for (unsigned step = 0; step < 100; ++step)
        expect(system.stepOneCycle().success,
               "post-boot register inspection program advances successfully");
    registers = system.getCpuRegisters();
    expect(registers.getRefB() == 0xAB,
           "DMG post-boot divider register starts at $AB");
    expect(registers.getRefC() == 0xFC && registers.getRefD() == 0xCF,
           "DMG post-boot background palette and joypad state are initialized");
    expect(registers.getRefE() == 0xE1 && registers.getRefH() == 0x91 &&
               registers.getRefL() == 0x00,
           "DMG post-boot IF, LCDC, and IE values are initialized");

    const uint64_t elapsedBeforeFrame = system.getElapsedTCycles();
    const RetStepFrame frame = system.stepFrame();
    expect(frame.success && frame.frameCompleted,
           "frame stepping completes while the CPU is running");
    expect(frame.tCyclesElapsed == GameBoySystem::DMG_FRAME_T_CYCLES,
           "one frame step advances 456 by 154 T-cycles");
    expect(system.getElapsedTCycles() ==
               elapsedBeforeFrame + GameBoySystem::DMG_FRAME_T_CYCLES,
           "system clock advances the requested frame duration");
}

void testFrameSteppingReturnsWhenCpuStops() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    rom[0x0100] = 0x10; // STOP
    rom[0x0101] = 0x00;

    GameBoySystem system(std::move(rom));
    const RetStepFrame frame = system.stepFrame();
    expect(frame.success && !frame.frameCompleted,
           "frame stepping reports that STOP prevented frame completion");
    expect(frame.tCyclesElapsed < GameBoySystem::DMG_FRAME_T_CYCLES,
           "frame stepping returns instead of looping forever during STOP");
}

void testPostBootInterruptsWaitForEi() {
    std::vector<uint8_t> rom(0x8000, 0);
    const uint8_t program[] = {
        0x3E, 0x01,       // LD A, VBlank interrupt mask
        0xE0, 0x0F,       // LDH [IF], A
        0xEA, 0xFF, 0xFF, // LD [IE], A
        0x06, 0x42,       // LD B, marker before EI
        0xFB, 0x00,       // EI; NOP
        0x16, 0x99,       // LD D, marker after interrupt return
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[0x100 + i] = program[i];
    rom[0x40] = 0x0E; // LD C, handler marker; RETI
    rom[0x41] = 0x77;
    rom[0x42] = 0xD9;
    GameBoySystem system(std::move(rom));
    for (unsigned i = 0; i < 40 && system.getCpuRegisters().getRefB() != 0x42; ++i)
        expect(system.stepOneCycle().success, "post-boot interrupt setup executes");
    auto registers = system.getCpuRegisters();
    expect(registers.getRefB() == 0x42 && registers.getRefC() == 0x13,
           "post-boot IME keeps pending IE/IF from entering a handler before EI");
    expect(registers.getRefSP() == 0xFFFE,
           "pending interrupts do not change the post-boot stack before EI");
    for (unsigned i = 0; i < 40 && system.getCpuRegisters().getRefD() != 0x99; ++i)
        expect(system.stepOneCycle().success, "EI and pending handler execute");
    registers = system.getCpuRegisters();
    expect(registers.getRefC() == 0x77 && registers.getRefD() == 0x99,
           "EI permits the pending handler and returns to the main program");
    expect(registers.getRefSP() == 0xFFFE, "RETI restores the post-boot stack");
}

}

int main() {
    testPostBootInterruptsWaitForEi();
    testCpuFetchesAndExecutesFromCartridgeThroughSystemBus();
    testEchoRamReadsAndWritesWorkRam();
    testTimerInterruptWakesHaltAndRunsHandler();
    testSystemTimerClocksApuFrameSequencer();
    testSystemBusMapsWaveRamToApu();
    testGameBoySystemAcceptsButtonInput();
    testPpuAdvancesAndExposesScanlineThroughSystemBus();
    testGameBoySystemExposesRenderedFrameBuffer();
    testDmgPostBootStartupAndFrameStepping();
    testFrameSteppingReturnsWhenCpuStops();
    std::cout << "Headless system tests passed\n";
}
