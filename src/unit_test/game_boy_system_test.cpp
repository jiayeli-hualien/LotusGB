#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "game_boy_system.hpp"
#include "../../tests/include/lcd_cadence_rom.hpp"
#include "../../tests/include/mbc1_multicart_rom.hpp"

namespace LOTUSGB::Test {
struct GameBoySystemTestAccess {
    static void setMbc1Multicart(GameBoySystem &system, bool value) {
        CartridgeTestAccess::setMbc1Multicart(system.cartridge, value);
    }
};
}

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

void testDmgBootRomOverlaysAndUnmapsBeforeCartridgeExecution() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    rom[0x0000] = 0x3E; // Cartridge opcode, hidden by the boot ROM.
    rom[0x0001] = 0x24;
    rom[0x0004] = 0x3E; // Visible after the boot ROM writes to FF50.
    rom[0x0005] = 0x99;

    std::vector<uint8_t> bootRom(0x100, 0x00);
    bootRom[0x0000] = 0x3E; // LD A, $01
    bootRom[0x0001] = 0x01;
    bootRom[0x0002] = 0xE0; // LDH [$FF50], A
    bootRom[0x0003] = 0x50;

    GameBoySystem system(std::move(rom), nullptr, nullptr,
                         std::move(bootRom));
    expect(system.getCpuRegisters().getRefPC() == 0,
           "boot ROM startup begins at the reset vector");

    for (unsigned cycle = 0; cycle < 10; ++cycle)
        expect(system.stepOneCycle().success,
               "boot ROM and cartridge handoff execute successfully");

    expect(system.getCpuRegisters().getRefA() == 0x99,
           "FF50 unmaps the boot ROM and exposes cartridge code");
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

void testVblankInterruptWakesHaltAndRunsHandler() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x31, 0x00, 0xC1,       // LD SP, $C100
        0xAF,                   // XOR A
        0xE0, 0x0F,             // LDH [IF], A (clear pending interrupts)
        0x3E, 0x01,             // LD A, VBlank interrupt mask
        0xEA, 0xFF, 0xFF,       // LD [IE], A (enable VBlank)
        0xFB,                   // EI
        0x76, 0x18, 0xFD,       // HALT; loop back to HALT after RETI
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[0x100 + i] = program[i];
    rom[0x40] = 0x06; // LD B, $42
    rom[0x41] = 0x42;
    rom[0x42] = 0xD9; // RETI

    GameBoySystem system(std::move(rom));
    for (unsigned cycle = 0; cycle < 20'000; ++cycle) {
        auto registers = system.getCpuRegisters();
        if (registers.getRefB() == 0x42 && registers.getRefSP() == 0xC100)
            break;
        expect(system.stepOneCycle().success,
               "PPU-generated VBlank interrupt system step succeeds");
    }

    auto registers = system.getCpuRegisters();
    expect(registers.getRefB() == 0x42,
           "PPU VBlank request wakes HALT and reaches the VBlank handler");
    expect(registers.getRefSP() == 0xC100,
           "VBlank handler returns through the interrupt stack frame");
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

void testVblankSteppingUsesFrameEventsAndSurvivesRestore() {
    std::vector<uint8_t> rom(0x8000, 0);
    const uint8_t program[] = {0x3E, 0xFF, 0xE0, 0x47, // BGP: black
                               0xC3, 0x04, 0x01};      // JP $0104
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x100);
    GameBoySystem system(std::move(rom));
    auto result = system.stepUntilVBlank(456 * 144 - 4);
    expect(result.success && !result.frameCompleted && result.tCyclesElapsed == 65660,
           "VBlank stepping stops at its budget without mistaking early LY for completion");
    result = system.stepUntilVBlank(4);
    expect(result.success && result.frameCompleted && result.tCyclesElapsed == 4,
           "the final M-cycle reaches the actual PPU frame event");
    expect(system.getCompletedFrameBuffer()[0] == 3,
           "VBlank stepping exposes the newly completed frame");
    auto saved = system.saveState();
    result = system.stepUntilVBlank();
    expect(result.success && result.frameCompleted &&
               result.tCyclesElapsed == GameBoySystem::DMG_FRAME_T_CYCLES,
           "starting in VBlank waits for the next full frame, not the current VBlank level");
    const auto future = system.saveState();
    system.loadState(saved.data(), saved.size());
    result = system.stepUntilVBlank();
    expect(result.success && result.frameCompleted &&
               result.tCyclesElapsed == GameBoySystem::DMG_FRAME_T_CYCLES &&
               system.saveState() == future,
           "restored VBlank stepping keeps its target callbacks and deterministic continuation");
    result = system.stepFrame();
    expect(result.success && result.frameCompleted &&
               result.tCyclesElapsed == GameBoySystem::DMG_FRAME_T_CYCLES,
           "fixed-duration stepping retains its original contract");
    for (uint64_t budget : {0, 6}) {
        bool rejected = false;
        try { system.stepUntilVBlank(budget); }
        catch (const std::invalid_argument &) { rejected = true; }
        expect(rejected, "VBlank step rejects a zero or partial-M-cycle budget");
    }
}

void testVblankSteppingBoundsDisabledStoppedAndLockedSystems() {
    auto off = GameBoySystem::createForTest(std::vector<uint8_t>(0x8000, 0));
    auto result = off.stepUntilVBlank(256);
    expect(result.success && !result.frameCompleted && result.tCyclesElapsed == 256,
           "LCD-off execution is bounded and explicitly reports no frame event");

    std::vector<uint8_t> rom(0x8000, 0);
    rom[0] = 0x10; // STOP
    auto stopped = GameBoySystem::createForTest(rom);
    result = stopped.stepUntilVBlank();
    expect(result.success && !result.frameCompleted && result.tCyclesElapsed < 70224,
           "STOP returns before a VBlank without an unbounded wait");
    rom[0] = 0xD3; // Invalid opcode hard-locks the CPU, not the PPU clock.
    auto locked = GameBoySystem::createForTest(rom);
    result = locked.stepUntilVBlank(256);
    expect(result.success && !result.frameCompleted && result.tCyclesElapsed == 256 &&
               locked.getCpuRegisters().getRefPC() == 1,
           "bounded VBlank stepping keeps system clocks running with a locked CPU");

    rom[0] = 0;
    rom[0x100] = 0xF3; // DI
    rom[0x101] = 0x76; // HALT
    GameBoySystem halted(std::move(rom));
    result = halted.stepUntilVBlank();
    expect(result.success && result.frameCompleted && result.tCyclesElapsed == 456 * 144,
           "HALT keeps clocking the PPU and reaches VBlank, unlike STOP");
}

void testDmgSystemDoesNotExposeCgbBankRegisters() {
    std::vector<uint8_t> rom(0x8000, 0);
    const uint8_t program[] = {
        0x3E, 0x01, 0xE0, 0x4F, // attempt VBK=1
        0xF0, 0x4F, 0x47,       // B=VBK
        0x3E, 0x07, 0xE0, 0x70, // attempt SVBK=7
        0xF0, 0x70, 0x4F,       // C=SVBK
        0x1E, 0x99, 0x18, 0xFE,
    };
    std::copy(std::begin(program), std::end(program), rom.begin());
    auto system = GameBoySystem::createForTest(std::move(rom));
    for (unsigned cycle = 0; cycle < 100 && system.getCpuRegisters().getRefE() != 0x99; ++cycle)
        expect(system.stepOneCycle().success, "DMG bank-register probe executes");
    auto registers = system.getCpuRegisters();
    expect(registers.getRefE() == 0x99 && registers.getRefB() == 0xFF && registers.getRefC() == 0xFF,
           "CGB memory foundations do not enable VBK/SVBK in the existing DMG system");
}

void testPresentationReasonsSeparateHostRepeatFromHardwareVblank() {
    std::vector<uint8_t> rom(0x8000, 0);
    const uint8_t readIf[] = {0xF0, 0x0F, 0x47, 0x18, 0xFB};
    std::copy(std::begin(readIf), std::end(readIf), rom.begin());
    auto off = GameBoySystem::createForTest(rom);
    for (unsigned i = 0; i < 2; ++i) {
        const auto result = off.stepForPresentation();
        expect(result.reason == PresentationReason::Repeat && result.tCyclesElapsed == 70224,
               "LCD-off presentation returns a bounded host repeat");
        expect((off.getCpuRegisters().getRefB() & 1) == 0,
               "host presentation repeats never synthesize a guest VBlank IRQ");
    }
    GameBoySystem visible(rom);
    auto result = visible.stepForPresentation();
    expect(result.reason == PresentationReason::VBlank && result.tCyclesElapsed == 65664,
           "presentation identifies actual first frame completion separately from repeats");
    result = visible.stepForPresentation();
    expect(result.reason == PresentationReason::VBlank && result.tCyclesElapsed == 70224,
           "steady presentation returns on the next real VBlank");
    // A state recorded under the previous fixed stepping policy needs no
    // migration: its hardware phase determines the next presentation event.
    GameBoySystem legacy(rom);
    expect(legacy.stepFrame().success, "legacy fixed step advances");
    legacy.takeAudioSamples();
    auto fixedState = legacy.saveState();
    GameBoySystem restored(rom);
    restored.loadState(fixedState.data(), fixedState.size());
    result = restored.stepForPresentation();
    expect(result.reason == PresentationReason::VBlank && result.tCyclesElapsed == 65664,
           "a fixed-policy state resumes at its actual next VBlank under presentation stepping");
    const auto referenceStep = legacy.stepUntilVBlank();
    expect(referenceStep.frameCompleted && referenceStep.tCyclesElapsed == result.tCyclesElapsed &&
               legacy.saveState() == restored.saveState() &&
               legacy.takeAudioSamples() == restored.takeAudioSamples(),
           "policy transition preserves the same hardware state and PCM at the shared event");
    rom[0] = 0x10;
    auto stopped = GameBoySystem::createForTest(std::move(rom));
    result = stopped.stepForPresentation();
    expect(result.reason == PresentationReason::CpuStopped && result.tCyclesElapsed < 70224,
           "CPU STOP is observable rather than classified as a healthy repeat");
}

void testAudioCountsActualCyclesAcrossLcdRestartStepping() {
    uint64_t fixedCycles = 0;
    for (bool toVblank : {false, true}) {
        GameBoySystem system(TestRom::lcdRestartCadence());
        size_t audioFrames = 0;
        unsigned incomplete = 0, shortCalls = 0;
        for (unsigned call = 0; call < 120; ++call) {
            const auto step = toVblank ? system.stepUntilVBlank() : system.stepFrame();
            expect(step.success && step.tCyclesElapsed > 0 && step.tCyclesElapsed <= 70224,
                   "LCD restart stepping stays bounded and preserves running CPU progress");
            incomplete += !step.frameCompleted;
            shortCalls += step.tCyclesElapsed < 70224;
            audioFrames += system.takeAudioSamples().size() / 2;
            expect(audioFrames == system.getElapsedTCycles() * 48000 / 4194304,
                   "LCD restart PCM count follows actual hardware time on every step");
        }
        if (!toVblank) {
            fixedCycles = system.getElapsedTCycles();
            expect(incomplete == 0 && shortCalls == 0 && fixedCycles == 120 * 70224ULL,
                   "fixed stepping keeps nominal time even while LCD is off");
        } else {
            expect(incomplete > 0 && shortCalls > 0 && system.getElapsedTCycles() < fixedCycles,
                   "VBlank restart cadence differs from fixed-rate frontend time and remains observable");
        }
        std::cout << "LCD restart system cadence (" << (toVblank ? "VBlank" : "fixed")
                  << "): " << system.getElapsedTCycles() << " T-cycles, " << audioFrames
                  << " PCM frames / 120 calls; incomplete=" << incomplete << '\n';
    }
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

void testSelectedJoypadLineWakesStopWithoutInterruptAcceptance() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x3E, 0x10,       // Select action buttons only.
        0xE0, 0x00,       // LDH [JOYP], A
        0x10, 0x00,       // STOP
        0x06, 0x42,       // LD B, $42 after wake
        0xF0, 0x0F,       // LDH A, [IF]
        0x18, 0xFE,       // Loop
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[0x100 + i] = program[i];

    GameBoySystem system(std::move(rom));
    bool stopped = false;
    for (unsigned cycle = 0; cycle < 20; ++cycle) {
        const auto step = system.stepOneCycle();
        expect(step.success, "STOP setup executes successfully");
        if (step.tCyclesElapsed == 0) {
            stopped = true;
            break;
        }
    }
    expect(stopped, "system enters STOP after configuring JOYP");
    const uint64_t stoppedAt = system.getElapsedTCycles();

    system.setButtonState(JoypadButton::Right, true);
    expect(system.stepOneCycle().tCyclesElapsed == 0,
           "an unselected direction line does not wake STOP");
    expect(system.getElapsedTCycles() == stoppedAt,
           "system time remains frozen without a selected low input line");

    system.setButtonState(JoypadButton::A, true);
    const auto wakeFetch = system.stepOneCycle();
    expect(wakeFetch.success && wakeFetch.fetchedNewInst &&
               wakeFetch.tCyclesElapsed == T_CYCLES_PER_M_CYCLE,
           "a selected action-line falling edge wakes STOP and resumes fetching");
    for (unsigned cycle = 0;
         cycle < 12 && system.getCpuRegisters().getRefA() != 0xF0; ++cycle)
        expect(system.stepOneCycle().success,
               "CPU continues executing after joypad wakes STOP");

    auto registers = system.getCpuRegisters();
    expect(registers.getRefB() == 0x42,
           "the instruction after STOP executes after joypad wake");
    expect((registers.getRefA() & INTERRUPT_JOYPAD) != 0,
           "the same selected line transition requests the joypad IF bit");
    expect(registers.getRefPC() >= 0x0100 && system.getElapsedTCycles() > stoppedAt,
           "wake resumes cartridge execution without interrupt-vector service");
}

void testStopRequiresFreshFallingJoypadEdgeWhenLineWasAlreadyLow() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x3E, 0x10,       // Select action buttons only.
        0xE0, 0x00,       // LDH [JOYP], A
        0xAF,             // XOR A
        0xE0, 0x0F,       // Clear the Joypad IF raised before STOP.
        0x10, 0x00,       // STOP while A is already held on a selected line.
        0xF0, 0x0F,       // Read IF after a fresh press wakes the CPU.
        0x06, 0x42,       // LD B, $42
        0x18, 0xFE,       // Loop
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[0x100 + i] = program[i];

    GameBoySystem system(std::move(rom));
    system.setButtonState(JoypadButton::A, true);
    bool stopped = false;
    for (unsigned cycle = 0; cycle < 32; ++cycle) {
        const auto step = system.stepOneCycle();
        expect(step.success, "held-input STOP setup executes successfully");
        if (step.tCyclesElapsed == 0) {
            stopped = true;
            break;
        }
    }
    expect(stopped, "a selected line already low does not prevent entering STOP");

    const uint64_t stoppedAt = system.getElapsedTCycles();
    expect(system.stepOneCycle().tCyclesElapsed == 0 &&
               system.getElapsedTCycles() == stoppedAt,
           "an already-low selected line does not wake STOP retroactively");

    system.setButtonState(JoypadButton::A, false);
    expect(system.stepOneCycle().tCyclesElapsed == 0,
           "releasing the held key raises the selected line without waking STOP");
    system.setButtonState(JoypadButton::A, true);
    const auto wakeFetch = system.stepOneCycle();
    expect(wakeFetch.success && wakeFetch.fetchedNewInst &&
               wakeFetch.tCyclesElapsed == T_CYCLES_PER_M_CYCLE,
           "a new selected-line falling edge wakes STOP and resumes instruction fetch");
    for (unsigned cycle = 0;
         cycle < 12 && system.getCpuRegisters().getRefB() != 0x42; ++cycle)
        expect(system.stepOneCycle().success,
               "CPU continues executing after a fresh edge wakes STOP");
    auto registers = system.getCpuRegisters();
    expect(registers.getRefB() == 0x42 &&
               (registers.getRefA() & INTERRUPT_JOYPAD) != 0,
           "the fresh wake edge requests Joypad IF and executes the next instruction");
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

void testCpuRunsHramDmaRoutineAndReturnsToRom() {
    std::vector<uint8_t> rom(0x8000, 0);
    const uint8_t routine[] = {
        0x3E, 0xC0,       // LD A, source page
        0xE0, 0x46,       // LDH [DMA], A
        0x3E, 40,         // LD A, wait counter
        0x3D,             // DEC A
        0x20, 0xFD,       // JR NZ, wait
        0xC9,             // RET to ROM after DMA releases the bus
    };
    for (size_t i = 0; i < sizeof(routine); ++i)
        rom[0x200 + i] = routine[i];
    const uint8_t program[] = {
        0xF3, 0xAF, 0xE0, 0x40, // DI; XOR A; disable LCD
        0x3E, 0x42, 0xEA, 0x00, 0xC0, // source first byte
        0x3E, 0x73, 0xEA, 0x9F, 0xC0, // source final byte
        0x21, 0x00, 0x02, // LD HL, ROM routine
        0x11, 0x80, 0xFF, // LD DE, HRAM destination
        0x06, static_cast<uint8_t>(sizeof(routine)), // LD B, byte count
        0x2A, 0x12, 0x13, 0x05, 0x20, 0xFA, // copy loop
        0xCD, 0x80, 0xFF, // CALL HRAM DMA routine
        0xFA, 0x00, 0xFE, 0x47, // LD A, [OAM]; LD B, A
        0xFA, 0x9F, 0xFE, 0x4F, // LD A, [OAM+159]; LD C, A
        0x16, 0x99, 0x18, 0xFE, // completion marker; loop
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[0x100 + i] = program[i];
    GameBoySystem system(std::move(rom));
    for (unsigned i = 0; i < 1500 && system.getCpuRegisters().getRefD() != 0x99; ++i)
        expect(system.stepOneCycle().success, "HRAM DMA program executes");
    auto registers = system.getCpuRegisters();
    expect(registers.getRefD() == 0x99, "CPU returns from HRAM DMA into ROM");
    expect(registers.getRefB() == 0x42 && registers.getRefC() == 0x73,
           "shared system clock completes DMA before the routine returns");
    expect(registers.getRefSP() == 0xFFFE, "HRAM wait routine preserves the call stack");
}

void testDisconnectedSerialWakesHaltAndRunsHandler() {
    std::vector<uint8_t> rom(0x8000, 0);
    const uint8_t program[] = {
        0xF3, 0xAF,       // DI; XOR A
        0xE0, 0x0F,       // clear IF
        0xE0, 0x01,       // SB = 0
        0x1E, 0x00,       // E = handler count
        0x3E, 0x08, 0xEA, 0xFF, 0xFF, // enable serial IRQ
        0x3E, 0x81, 0xE0, 0x02, // request internal-clock transfer
        0xFB, 0x76,       // EI; HALT until completion
        0xF0, 0x02, 0x47, // B = completed SC
        0xF0, 0x01, 0x4F, // C = received SB
        0xF0, 0x0F, 0xE6, 0x08, 0x67, // H = serial IF after acknowledgement
        0x16, 0x99, 0x18, 0xFE, // completion marker; loop
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[0x100 + i] = program[i];
    rom[0x58] = 0x1C; // INC E
    rom[0x59] = 0xD9; // RETI
    GameBoySystem system(std::move(rom));
    for (unsigned i = 0; i < 2000 && system.getCpuRegisters().getRefD() != 0x99; ++i)
        expect(system.stepOneCycle().success, "serial/HALT system program executes");
    auto registers = system.getCpuRegisters();
    expect(registers.getRefD() == 0x99 && registers.getRefE() == 1,
           "serial completion wakes HALT and executes the serial vector once");
    expect(registers.getRefB() == 0x7F && registers.getRefC() == 0xFF,
           "CPU sees completed internal transfer and disconnected FF data");
    expect(registers.getRefH() == 0 && registers.getRefSP() == 0xFFFE,
           "serial handler acknowledges IF and RETI restores the stack");
    expect(system.getElapsedTCycles() >= 4000 && system.getElapsedTCycles() < 6000,
           "serial byte completes at the normal DMG clock rate while CPU is halted");
}

void testSerialDiagnosticStateCapturesCpuAndInterruptRegisters() {
    std::vector<uint8_t> rom(0x8000, 0);
    const uint8_t program[] = {
        0x3E, 0xA5,             // LD A, $A5
        0xE0, 0x01,             // SB = $A5
        0x3E, 0x81,             // A = internal-clock transfer start
        0xE0, 0x02,             // SC = $81
        0x3E, 0x08,             // A = serial IF bit
        0xE0, 0x0F,             // IF = $08
        0x18, 0xFE,             // JR $ (keep the transfer pending)
    };
    std::copy(std::begin(program), std::end(program), rom.begin());

    auto system = GameBoySystem::createForTest(std::move(rom));
    for (unsigned cycle = 0; cycle < 32; ++cycle)
        expect(system.stepOneCycle().success,
               "serial diagnostic fixture reaches its pending transfer");

    const SerialDiagnosticState state = system.getSerialDiagnosticState();
    expect(state.elapsedTCycles > 0 && state.programCounter > 0,
           "serial diagnostic snapshot reports elapsed system time and CPU PC");
    expect(state.serialData == 0xA5 && (state.serialControl & 0x81) == 0x81,
           "serial diagnostic snapshot reports SB and active internal-clock SC");
    expect((state.interruptFlags & 0x08) != 0 && state.interruptEnable == 0,
           "serial diagnostic snapshot reports IF separately from IE");
    expect(!state.halted && !state.stopped,
           "serial diagnostic snapshot reports CPU run state");
}

void testSerialClockIsIndependentOfDividerWrites() {
    std::vector<uint8_t> rom(0x8000, 0);
    const uint8_t program[] = {
        0xF3, 0xAF, 0xE0, 0x01, // DI; SB = 0
        0x3E, 0x81, 0xE0, 0x02, // start serial
        // Repeatedly reset DIV while polling for serial completion.
        0xAF, 0xE0, 0x04, 0xF0, 0x02, 0xCB, 0x7F, 0x20, 0xF7,
        0xF0, 0x01, 0x4F, // C = received data
        0xF0, 0x0F, 0xE6, 0x08, 0x67, // H = serial request with IME disabled
        0x16, 0x99, 0x18, 0xFE,
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[0x100 + i] = program[i];
    GameBoySystem system(std::move(rom));
    for (unsigned i = 0; i < 2000 && system.getCpuRegisters().getRefD() != 0x99; ++i)
        expect(system.stepOneCycle().success, "serial/DIV polling system program executes");
    auto registers = system.getCpuRegisters();
    expect(registers.getRefD() == 0x99 && registers.getRefC() == 0xFF,
           "frequent DIV resets do not prevent serial transfer completion");
    expect(registers.getRefH() == 8,
           "serial completion latches IF even with IME and serial IE disabled");
}

void testLycZeroHandlerRunsBeforeVblankEnds() {
    std::vector<uint8_t> rom(0x8000, 0);
    const uint8_t program[] = {
        0xF3, 0xAF, 0xE0, 0x0F, // DI; clear IF
        0x3E, 0x02, 0xEA, 0xFF, 0xFF, // enable LCD STAT IRQ
        0x3E, 0x40, 0xE0, 0x41, // enable LYC source, LYC defaults to 0
        0xFB, 0x00, // EI; NOP
        0x76, 0x18, 0xFD, // HALT; loop
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[0x100 + i] = program[i];
    const uint8_t handler[] = {
        0xF0, 0x41, 0xE6, 0x03, // A = STAT mode
        0xFE, 0x01, 0x20, 0x06, // CP 1; skip observations outside VBlank
        0x47,                   // B = mode 1
        0xF0, 0x44, 0x4F,       // C = readable LY
        0x16, 0x99,             // D = completion marker
        0xD9,                   // RETI, including startup LYC interrupts
    };
    for (size_t i = 0; i < sizeof(handler); ++i)
        rom[0x48 + i] = handler[i];
    GameBoySystem system(std::move(rom));
    for (unsigned cycle = 0; cycle < 20000 && system.getCpuRegisters().getRefD() != 0x99;
         ++cycle)
        expect(system.stepOneCycle().success, "LYC-zero interrupt system program executes");
    auto registers = system.getCpuRegisters();
    expect(registers.getRefD() == 0x99 && registers.getRefB() == 1 && registers.getRefC() == 0,
           "CPU receives LYC=0 while readable LY is zero and STAT is still in VBlank");
}

void testPpuAccessWindowsSupportSafeCpuUpdates() {
    std::vector<uint8_t> rom(0x8000, 0);
    const uint8_t program[] = {
        0x3E, 0x12,             // A = original VRAM value
        0xEA, 0x00, 0x80,       // [VRAM] = A while LCD is disabled
        0x3E, 0x34,             // A = original OAM value
        0xEA, 0x00, 0xFE,       // [OAM] = A while LCD is disabled
        0x3E, 0x91, 0xE0, 0x40, // Enable LCD and BG
        // Poll until Mode 3.
        0xF0, 0x41, 0xE6, 0x03, 0xFE, 0x03, 0x20, 0xF8,
        0x3E, 0x99,             // A = attempted VRAM replacement
        0xEA, 0x00, 0x80,       // Blocked Mode 3 write must be discarded
        0x3E, 0x88,             // A = attempted OAM replacement
        0xEA, 0x00, 0xFE,       // Blocked Mode 3 write must be discarded
        0xF0, 0x41, 0x4F,       // C = STAT after both writes
        // Poll until Mode 0, then verify old values before valid updates.
        0xF0, 0x41, 0xE6, 0x03, 0xFE, 0x00, 0x20, 0xF8,
        0xFA, 0x00, 0x80, 0x47, // B = original VRAM value
        0xFA, 0x00, 0xFE, 0x57, // D = original OAM value
        0x3E, 0x56,             // A = valid VRAM update
        0xEA, 0x00, 0x80,       // Mode 0 VRAM write succeeds
        0x3E, 0x78,             // A = valid OAM update
        0xEA, 0x00, 0xFE,       // Mode 0 OAM write succeeds
        0xFA, 0x00, 0x80, 0x67, // H = updated VRAM value
        0xFA, 0x00, 0xFE, 0x6F, // L = updated OAM value
        0x1E, 0x99,             // E = completion marker
        0x18, 0xFE,             // Loop
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[i] = program[i];

    GameBoySystem system = GameBoySystem::createForTest(std::move(rom));
    for (unsigned cycle = 0;
         cycle < 500 && system.getCpuRegisters().getRefE() != 0x99; ++cycle)
        expect(system.stepOneCycle().success,
               "CPU/PPU bus access program advances successfully");

    auto registers = system.getCpuRegisters();
    expect(registers.getRefE() == 0x99,
           "CPU continues past VRAM and OAM writes rejected in Mode 3");
    expect((registers.getRefC() & 0x03) == 3,
           "rejected writes do not wait until the PPU leaves Mode 3");
    expect(registers.getRefB() == 0x12 && registers.getRefD() == 0x34,
           "rejected writes are discarded rather than replayed in Mode 0");
    expect(registers.getRefH() == 0x56 && registers.getRefL() == 0x78,
           "CPU writes to VRAM and OAM succeed after polling until Mode 0");
}

std::vector<uint8_t> makeSerialPeerRom(uint8_t outgoing, bool master, bool fast, bool doubleSpeed) {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x143] = 0x80; rom[0x147] = 3; rom[0x149] = 2;
    const uint8_t program[] = {
        0xF3, 0x3E, 0x0A, 0xEA, 0, 0, // Enable battery RAM; keep IME off.
        0xAF, 0xE0, 0x0F, 0x3E, 8, 0xEA, 0xFF, 0xFF, // Clear IF; IE=serial.
        0x3E, outgoing, 0xE0, 1,
        0x3E, static_cast<uint8_t>(master ? (fast ? 0x83 : 0x81) : 0x80), 0xE0, 2,
        0x76,                         // HALT until the eighth serial edge.
        0xF0, 1, 0xEA, 0x30, 0xA0,   // Received byte.
        0xF0, 2, 0xEA, 0x31, 0xA0,   // Transfer-complete SC state.
        0x18, 0xFE
    };
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x150);
    if (doubleSpeed) {
        rom[0x101] = 0; rom[0x102] = 3;
        const uint8_t speedSwitch[] = {0xF3, 0x3E, 1, 0xE0, 0x4D, 0x10, 0, 0xC3, 0x50, 1};
        std::copy(std::begin(speedSwitch), std::end(speedSwitch), rom.begin() + 0x300);
    }
    return rom;
}

void testTwoSystemsSerialExchangeAndPairedRestore() {
    struct Mode { HardwareModel model; bool fast, doubleSpeed; };
    for (const auto mode : {Mode{HardwareModel::Dmg, false, false},
                           Mode{HardwareModel::CgbE, false, false},
                           Mode{HardwareModel::CgbE, true, false},
                           Mode{HardwareModel::CgbE, false, true},
                           Mode{HardwareModel::CgbE, true, true}}) {
        const auto model = mode.model;
        const auto masterRom = makeSerialPeerRom(0xA5, true, mode.fast, mode.doubleSpeed);
        const auto slaveRom = makeSerialPeerRom(0x3C, false, mode.fast, mode.doubleSpeed);
        GameBoySystem master(masterRom, model), slave(slaveRom, model);
        GameBoySystem replayMaster(masterRom, model), replaySlave(slaveRom, model);
        unsigned edges = 0, replayEdges = 0;
        std::vector<uint64_t> edgeTimes;
        uint64_t masterSteps = 0;
        const auto stepMaster = [&] { ++masterSteps; return master.stepOneCycle().success; };
        master.setSerialClockCallback([&](bool outgoing) {
            const bool incoming = slave.getSerialOutgoingBit();
            slave.clockSerialExternalBit(outgoing); ++edges;
            // All tested edges align to M-cycle ends. Count host-driven steps:
            // getElapsedTCycles is updated at different points for bus and HALT steps.
            edgeTimes.push_back(masterSteps); return incoming;
        });
        // Install before load: committing guest state must not replace host wiring.
        replayMaster.setSerialClockCallback([&](bool outgoing) {
            const bool incoming = replaySlave.getSerialOutgoingBit();
            replaySlave.clockSerialExternalBit(outgoing); ++replayEdges; return incoming;
        });
        unsigned cycles = 0;
        for (; cycles < 10000 && edges < 3; ++cycles)
            expect(slave.stepOneCycle().success && stepMaster(),
                   "paired CPU programs arm external slave before internal master");
        expect(edges == 3, "two-system link reaches a partial-byte snapshot boundary");
        const auto masterState = master.saveState(), slaveState = slave.saveState();
        auto *masterRam = replayMaster.getSaveRamData();
        auto *slaveRam = replaySlave.getSaveRamData();
        replayMaster.loadState(masterState.data(), masterState.size());
        replaySlave.loadState(slaveState.data(), slaveState.size());
        expect(masterRam == replayMaster.getSaveRamData() && slaveRam == replaySlave.getSaveRamData(),
               "paired link restore preserves published SRAM pointers");
        for (unsigned cycle = 0; cycle < 2048; ++cycle)
            expect(slave.stepOneCycle().success && stepMaster() &&
                       replaySlave.stepOneCycle().success && replayMaster.stepOneCycle().success,
                   "original and restored pairs run serial IRQ/HALT wake to completion");
        expect(edges == 8 && replayEdges == 5 && masterRam[0x30] == 0x3C && slaveRam[0x30] == 0xA5 &&
                   !(masterRam[0x31] & 0x80) && !(slaveRam[0x31] & 0x80),
               "both CPUs wake HALT after byte exchange and restored host wiring supplies exactly five remaining edges");
        const uint64_t bitPeriod = (mode.fast ? 16 : 512) / (mode.doubleSpeed ? 2 : 1);
        for (size_t edge = 1; edge < edgeTimes.size(); ++edge)
            expect((edgeTimes[edge] - edgeTimes[edge - 1]) * (mode.doubleSpeed ? 2 : 4) == bitPeriod,
                   "linked CGB normal/fast clocks retain all four documented base-time rates");
        expect(master.takeAudioSamples() == replayMaster.takeAudioSamples() &&
                   slave.takeAudioSamples() == replaySlave.takeAudioSamples() &&
                   master.saveState() == replayMaster.saveState() && slave.saveState() == replaySlave.saveState(),
               "paired partial-transfer snapshots replay exact PCM and complete state");
    }
}

class TestRomAssembler {
public:
    void bytes(std::initializer_list<uint8_t> values) {
        code.insert(code.end(), values.begin(), values.end());
    }

    void label(const std::string &name) {
        if (!labels.emplace(name, code.size()).second)
            throw std::runtime_error("duplicate synthetic ROM label: " + name);
    }

    void jr(uint8_t opcode, const std::string &target) {
        bytes({opcode, 0});
        relativeFixups.emplace_back(code.size() - 1, target);
    }

    void call(const std::string &target) {
        bytes({0xCD, 0, 0});
        absoluteFixups.emplace_back(code.size() - 2, target);
    }

    std::vector<uint8_t> makeMbc1BatteryRom(const char *title) const {
        std::vector<uint8_t> program = code;
        for (const auto &fixup : relativeFixups) {
            const auto found = labels.find(fixup.second);
            if (found == labels.end())
                throw std::runtime_error("missing synthetic ROM label: " + fixup.second);
            const int offset = static_cast<int>(found->second) -
                               static_cast<int>(fixup.first + 1);
            if (offset < -128 || offset > 127)
                throw std::runtime_error("synthetic ROM JR is out of range: " + fixup.second);
            program[fixup.first] = static_cast<uint8_t>(static_cast<int8_t>(offset));
        }
        for (const auto &fixup : absoluteFixups) {
            const auto found = labels.find(fixup.second);
            if (found == labels.end())
                throw std::runtime_error("missing synthetic ROM label: " + fixup.second);
            const uint16_t address = static_cast<uint16_t>(0x0150 + found->second);
            program[fixup.first] = static_cast<uint8_t>(address & 0xFF);
            program[fixup.first + 1] = static_cast<uint8_t>(address >> 8);
        }

        std::vector<uint8_t> rom(0x8000, 0);
        rom[0x100] = 0xC3;
        rom[0x101] = 0x50;
        rom[0x102] = 0x01;
        const size_t titleLength = std::min<size_t>(11, std::char_traits<char>::length(title));
        std::copy(title, title + titleLength, rom.begin() + 0x134);
        rom[0x147] = 0x03; // MBC1 + RAM + battery.
        rom[0x148] = 0x00; // 32 KiB ROM.
        rom[0x149] = 0x02; // 8 KiB SRAM.
        std::copy(program.begin(), program.end(), rom.begin() + 0x150);

        uint8_t headerChecksum = 0;
        for (size_t address = 0x134; address <= 0x14C; ++address)
            headerChecksum = static_cast<uint8_t>(headerChecksum - rom[address] - 1);
        rom[0x14D] = headerChecksum;
        uint32_t globalChecksum = 0;
        for (size_t address = 0; address < rom.size(); ++address) {
            if (address != 0x14E && address != 0x14F)
                globalChecksum += rom[address];
        }
        rom[0x14E] = static_cast<uint8_t>(globalChecksum >> 8);
        rom[0x14F] = static_cast<uint8_t>(globalChecksum & 0xFF);
        return rom;
    }

private:
    std::vector<uint8_t> code;
    std::map<std::string, size_t> labels;
    std::vector<std::pair<size_t, std::string>> relativeFixups;
    std::vector<std::pair<size_t, std::string>> absoluteFixups;
};

constexpr uint16_t POKEMON_TEST_TRACE_COUNT = 0xA0F0;
constexpr uint16_t POKEMON_TEST_TRACE = 0xA100;
constexpr uint16_t POKEMON_TEST_ACTION_LOG = 0xA200;

std::vector<uint8_t> makePokemonBattleProbeRom(bool host, bool driftBeforeBattle = false,
                                               bool includeLongPayload = true,
                                               bool corruptSeedBeforeSnapshot = false) {
    TestRomAssembler rom;
    const auto storeA = [&](uint16_t address) {
        rom.bytes({0xEA, static_cast<uint8_t>(address & 0xFF),
                   static_cast<uint8_t>(address >> 8)});
    };
    const auto loadA = [&](uint16_t address) {
        rom.bytes({0xFA, static_cast<uint8_t>(address & 0xFF),
                   static_cast<uint8_t>(address >> 8)});
    };

    rom.bytes({0xF3, 0x31, 0xFE, 0xFF, // DI; SP = $FFFE.
               0x3E, 0x0A, 0xEA, 0x00, 0x00}); // Enable battery SRAM.
    rom.bytes({0x21, 0x20, 0xA0}); // HL = seed table.
    const std::array<uint8_t, 10> hostSeeds = {
        0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x22, 0x44};
    const std::array<uint8_t, 10> clientSeeds = {
        0x01, 0x03, 0x05, 0x07, 0x09, 0x0B, 0x0D, 0x0F, 0x11, 0x13};
    for (const uint8_t seed : (host ? hostSeeds : clientSeeds))
        rom.bytes({0x3E, seed, 0x22}); // LD A,seed; LD (HL+),A.

    if (includeLongPayload) {
        // Give each peer's long payload a distinct deterministic pattern:
        // payload[i] = (37 * (i & 255) + roleSalt) mod 256.
        // The two salt values make the later SRAM receive check bidirectional.
        rom.bytes({0x21, 0x00, 0xA3, 0x06, 0x02}); // HL = $A300; B = two 256-byte blocks.
        rom.label("payload_fill_block");
        rom.bytes({0x0E, 0x00}); // C = 0.
        rom.label("payload_fill_byte");
        rom.bytes({0x79, 0x57, 0x87, 0x87, 0x5F, // DE = i; E = 4*i.
                   0x7A, 0x87, 0x87, 0x87, 0x87, 0x87, 0x83, 0x82,
                   0xC6, static_cast<uint8_t>(host ? 0x51 : 0x93),
                   0x22, 0x0C, 0x79, 0xA7});
        rom.jr(0x20, "payload_fill_byte");
        rom.bytes({0x05});
        rom.jr(0x20, "payload_fill_block");
    }

    rom.bytes({0x3E, 0x11});
    storeA(0xA010); // Startup checkpoint.

    if (host) {
        rom.bytes({0x21, 0x20, 0xA0, 0x06, 0x0A}); // HL = table; B = ten bytes.
        rom.label("seed_host_byte");
        rom.bytes({0x7E, 0xE0, 0x01, 0x3E, 0x81, 0xE0, 0x02});
        rom.label("seed_host_wait");
        rom.bytes({0xF0, 0x02, 0xCB, 0x7F});
        rom.jr(0x20, "seed_host_wait");
        rom.bytes({0x23, 0x05});
        rom.jr(0x20, "seed_host_byte");
    } else {
        rom.bytes({0x21, 0x20, 0xA0, 0x06, 0x0A}); // HL = table; B = ten bytes.
        rom.label("seed_client_byte");
        rom.bytes({0x3E, 0xFF, 0xE0, 0x01, 0x3E, 0x80, 0xE0, 0x02});
        rom.label("seed_client_wait");
        rom.bytes({0xF0, 0x02, 0xCB, 0x7F});
        rom.jr(0x20, "seed_client_wait");
        rom.bytes({0xF0, 0x01, 0x22, 0x05});
        rom.jr(0x20, "seed_client_byte");
    }

    if (!host && corruptSeedBeforeSnapshot) {
        loadA(0xA023);
        rom.bytes({0xEE, 0x01}); // Deliberate corrupted received seed for negative control.
        storeA(0xA023);
    }
    rom.bytes({0x21, 0x20, 0xA0, 0x11, 0x80, 0xA0, 0x06, 0x0A});
    rom.label("seed_snapshot_copy");
    rom.bytes({0x2A, 0x12, 0x13, 0x05}); // Copy exchanged table before battle updates it.
    rom.jr(0x20, "seed_snapshot_copy");
    rom.bytes({0x3E, 0x22});
    storeA(0xA011); // Exact post-exchange table snapshot is ready.

    if (includeLongPayload) {
        rom.bytes({0x21, 0x00, 0xA3, // HL = outgoing payload.
                   0x11, 0x00, 0xA5, // DE = incoming payload log.
                   0x01, 0x00, 0x02}); // BC = 512 transfers.
        const std::string payloadLoop = host ? "payload_host_byte" : "payload_client_byte";
        const std::string payloadWait = host ? "payload_host_wait" : "payload_client_wait";
        rom.label(payloadLoop);
        rom.bytes({0x7E, 0xE0, 0x01, 0x3E,
                   static_cast<uint8_t>(host ? 0x81 : 0x80), 0xE0, 0x02});
        rom.label(payloadWait);
        rom.bytes({0xF0, 0x02, 0xCB, 0x7F});
        rom.jr(0x20, payloadWait);
        rom.bytes({0xF0, 0x01, 0x12, // Save the peer's byte to SRAM at DE.
                   0x23, 0x13, 0x0B, 0x78, 0xB1}); // Advance pointers; BC--.
        rom.jr(0x20, payloadLoop);
    }

    rom.bytes({0x3E, 0x33});
    storeA(0xA013); // 512-byte party-like payload transfer completed (or was omitted).

    rom.bytes({0x3E, 0x00});
    storeA(0xA030); // Battle RNG index.
    storeA(POKEMON_TEST_TRACE_COUNT);
    storeA(0xA033); // Completed turns.
    storeA(0xA034); // Hits.
    storeA(0xA035); // Critical hits.
    storeA(0xA036); // Total damage, low byte.
    storeA(0xA038); // Misses.
    storeA(0xA03A); // Peer-action protocol error.
    if (driftBeforeBattle)
        rom.call("battle_random"); // Deliberate off-by-one negative control.
    rom.bytes({0x3E, 120});
    storeA(0xA031); // Player 1 HP.
    storeA(0xA032); // Player 2 HP.
    rom.bytes({0x21, 0x00, 0xA2, 0x06, 0x08}); // Action log; eight turns max.

    rom.label("battle_turn");
    loadA(0xA031);
    rom.bytes({0xA7});
    rom.jr(0x28, "battle_finish");
    loadA(0xA032);
    rom.bytes({0xA7});
    rom.jr(0x28, "battle_finish");

    // Exchange one selected-action byte before resolving each synthetic turn.
    loadA(0xA033);
    rom.bytes({0xC6, static_cast<uint8_t>(host ? 0x10 : 0x20), 0x57}); // D = our move.
    loadA(0xA033);
    rom.bytes({0xC6, static_cast<uint8_t>(host ? 0x20 : 0x10), 0x4F}); // C = peer move.
    rom.bytes({0x7A, 0xE0, 0x01, 0x3E, static_cast<uint8_t>(host ? 0x81 : 0x80),
               0xE0, 0x02});
    const std::string actionWait = host ? "battle_action_host_wait" : "battle_action_client_wait";
    const std::string actionValid = host ? "battle_action_host_valid" : "battle_action_client_valid";
    rom.label(actionWait);
    rom.bytes({0xF0, 0x02, 0xCB, 0x7F});
    rom.jr(0x20, actionWait);
    rom.bytes({0xF0, 0x01, 0x77, 0x23, 0xB9}); // Record and compare peer action.
    rom.jr(0x28, actionValid);
    rom.bytes({0x3E, 0x01});
    storeA(0xA03A);
    rom.label(actionValid);

    rom.bytes({0x11, 0x32, 0xA0}); // Player 1 attacks player 2.
    rom.call("battle_attack");
    loadA(0xA032);
    rom.bytes({0xA7});
    rom.jr(0x28, "battle_skip_player_two_move");
    rom.bytes({0x11, 0x31, 0xA0}); // Player 2 attacks player 1 if still alive.
    rom.call("battle_attack");
    rom.label("battle_skip_player_two_move");
    loadA(0xA033);
    rom.bytes({0x3C});
    storeA(0xA033);
    rom.bytes({0x05});
    rom.jr(0x20, "battle_turn");

    rom.label("battle_finish");
    loadA(0xA03A);
    rom.bytes({0xA7});
    rom.jr(0x20, "battle_failed");
    rom.bytes({0x3E, 0x55});
    rom.jr(0x18, "battle_store_result");
    rom.label("battle_failed");
    rom.bytes({0x3E, 0xEE});
    rom.label("battle_store_result");
    storeA(0xA012);
    rom.label("battle_stop");
    rom.jr(0x18, "battle_stop");

    rom.label("battle_attack");
    rom.bytes({0x1A, 0xA7}); // Skip attacks against a fainted target.
    rom.jr(0x28, "battle_attack_done");
    rom.call("battle_random");
    rom.bytes({0xFE, 0xC0}); // Hit threshold.
    rom.jr(0x30, "battle_attack_miss");
    loadA(0xA034);
    rom.bytes({0x3C});
    storeA(0xA034);
    rom.call("battle_random");
    rom.bytes({0xFE, 0xE0});
    rom.jr(0x30, "battle_attack_critical");
    rom.call("battle_random");
    rom.bytes({0xE6, 0x03, 0xC6, 0x04}); // Damage = 4 + (random & 3).
    rom.jr(0x18, "battle_apply_damage");
    rom.label("battle_attack_critical");
    loadA(0xA035);
    rom.bytes({0x3C});
    storeA(0xA035);
    rom.bytes({0x3E, 0x0C}); // Critical hit damage = 12.
    rom.label("battle_apply_damage");
    rom.bytes({0x4F}); // C = damage.
    loadA(0xA036);
    rom.bytes({0x81});
    storeA(0xA036);
    rom.bytes({0x1A, 0x91}); // Target HP -= damage.
    rom.jr(0x30, "battle_store_hp");
    rom.bytes({0xAF}); // Clamp HP to zero on a knockout.
    rom.label("battle_store_hp");
    rom.bytes({0x12, 0xC9});
    rom.label("battle_attack_done");
    rom.bytes({0xC9});
    rom.label("battle_attack_miss");
    loadA(0xA038);
    rom.bytes({0x3C});
    storeA(0xA038);
    rom.bytes({0xC9});

    rom.label("battle_random");
    rom.bytes({0xC5, 0xD5, 0xE5}); // Preserve BC, DE and HL; A returns the draw.
    loadA(0xA030);
    rom.bytes({0x4F, 0x06, 0x00, 0x21, 0x20, 0xA0, 0x09, 0x7E, 0x57});
    loadA(POKEMON_TEST_TRACE_COUNT);
    rom.bytes({0x4F, 0x06, 0x00, 0x21,
               static_cast<uint8_t>(POKEMON_TEST_TRACE & 0xFF),
               static_cast<uint8_t>(POKEMON_TEST_TRACE >> 8), 0x09, 0x72});
    loadA(POKEMON_TEST_TRACE_COUNT);
    rom.bytes({0x3C});
    storeA(POKEMON_TEST_TRACE_COUNT);
    loadA(0xA030);
    rom.bytes({0x3C});
    storeA(0xA030);
    rom.bytes({0xFE, 0x09});
    rom.jr(0x20, "battle_random_return");
    rom.bytes({0xAF});
    storeA(0xA030);
    rom.bytes({0x21, 0x20, 0xA0, 0x06, 0x09});
    rom.label("battle_random_refresh");
    rom.bytes({0x7E, 0x4F, 0x87, 0x87, 0x81, 0x3C, 0x22, 0x05});
    rom.jr(0x20, "battle_random_refresh");
    rom.label("battle_random_return");
    rom.bytes({0x7A, 0xE1, 0xD1, 0xC1, 0xC9});

    return rom.makeMbc1BatteryRom(host ? "LGB RNG HOST" : "LGB RNG CLIENT");
}

struct PokemonBattleReference {
    std::array<uint8_t, 10> initialSeeds{};
    std::array<uint8_t, 10> seeds{};
    std::vector<uint8_t> trace;
    uint8_t rngIndex = 0;
    uint8_t playerOneHp = 120;
    uint8_t playerTwoHp = 120;
    uint8_t turns = 0;
    uint8_t hits = 0;
    uint8_t criticalHits = 0;
    uint8_t misses = 0;
    uint8_t totalDamage = 0;
    std::vector<uint8_t> actionsReceivedByHost;
    std::vector<uint8_t> actionsReceivedByClient;
};

struct PokemonBattlePairRun {
    std::array<uint8_t, 0x2000> hostSram{};
    std::array<uint8_t, 0x2000> clientSram{};
    unsigned serialEdges = 0;
};

PokemonBattleReference simulatePokemonBattleReference() {
    PokemonBattleReference result;
    result.seeds = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x22, 0x44};
    result.initialSeeds = result.seeds;
    const auto battleRandom = [&result]() {
        const uint8_t value = result.seeds[result.rngIndex];
        result.trace.push_back(value);
        ++result.rngIndex;
        if (result.rngIndex == 9) {
            result.rngIndex = 0;
            for (size_t seed = 0; seed < 9; ++seed)
                result.seeds[seed] = static_cast<uint8_t>(5 * result.seeds[seed] + 1);
        }
        return value;
    };
    const auto attack = [&result, &battleRandom](uint8_t &targetHp) {
        if (targetHp == 0)
            return;
        if (battleRandom() >= 0xC0) {
            ++result.misses;
            return;
        }
        ++result.hits;
        uint8_t damage = 0;
        if (battleRandom() >= 0xE0) {
            ++result.criticalHits;
            damage = 12;
        } else {
            damage = static_cast<uint8_t>(4 + (battleRandom() & 0x03));
        }
        result.totalDamage = static_cast<uint8_t>(result.totalDamage + damage);
        targetHp = targetHp > damage ? static_cast<uint8_t>(targetHp - damage) : 0;
    };

    for (uint8_t turn = 0; turn < 8; ++turn) {
        if (result.playerOneHp == 0 || result.playerTwoHp == 0)
            break;
        result.actionsReceivedByHost.push_back(static_cast<uint8_t>(0x20 + turn));
        result.actionsReceivedByClient.push_back(static_cast<uint8_t>(0x10 + turn));
        attack(result.playerTwoHp);
        if (result.playerTwoHp != 0)
            attack(result.playerOneHp);
        ++result.turns;
    }
    return result;
}

void testPokemonBattleProbeRomSynchronizesSeedsAndBattleState() {
    const auto runPair = [](bool driftBeforeBattle, bool corruptSeed, bool includeLongPayload,
                            unsigned maxCycles, bool stopAfterSeedExchange = false) {
        const auto hostRom = makePokemonBattleProbeRom(true, false, includeLongPayload);
        const auto clientRom = makePokemonBattleProbeRom(false, driftBeforeBattle,
                                                         includeLongPayload, corruptSeed);
        GameBoySystem host(hostRom, HardwareModel::Dmg);
        GameBoySystem client(clientRom, HardwareModel::Dmg);
        auto *hostRam = host.getSaveRamData();
        auto *clientRam = client.getSaveRamData();
        unsigned serialEdges = 0;
        host.setSerialClockCallback([&](bool outgoing) {
            const bool incoming = client.getSerialOutgoingBit();
            client.clockSerialExternalBit(outgoing);
            ++serialEdges;
            return incoming;
        });
        const auto completed = [&] {
            if (stopAfterSeedExchange)
                return hostRam[0x11] == 0x22 && clientRam[0x11] == 0x22;
            if (driftBeforeBattle && hostRam[0x13] == 0x33 && clientRam[0x13] == 0x33 &&
                hostRam[0x33] != clientRam[0x33])
                return true;
            return hostRam[0x12] == 0x55 && clientRam[0x12] == 0x55;
        };
        for (unsigned cycle = 0; cycle < maxCycles && !completed(); ++cycle) {
            expect(client.stepOneCycle().success && host.stepOneCycle().success,
                   "synthetic Pokémon battle peers execute successfully");
        }
        PokemonBattlePairRun run;
        std::copy_n(hostRam, run.hostSram.size(), run.hostSram.begin());
        std::copy_n(clientRam, run.clientSram.size(), run.clientSram.begin());
        run.serialEdges = serialEdges;
        return run;
    };

    const auto expected = simulatePokemonBattleReference();
    const auto normalRun = runPair(false, false, true, 700'000);
    const auto *hostRam = normalRun.hostSram.data();
    const auto *clientRam = normalRun.clientSram.data();
    if (hostRam[0x10] != 0x11 || clientRam[0x10] != 0x11 ||
        hostRam[0x11] != 0x22 || clientRam[0x11] != 0x22)
        std::cerr << "long probe markers=" << static_cast<unsigned>(hostRam[0x10]) << ","
                  << static_cast<unsigned>(clientRam[0x10]) << ","
                  << static_cast<unsigned>(hostRam[0x11]) << ","
                  << static_cast<unsigned>(clientRam[0x11]) << " edges="
                  << normalRun.serialEdges << '\n';
    expect(hostRam[0x10] == 0x11 && clientRam[0x10] == 0x11 &&
               hostRam[0x11] == 0x22 && clientRam[0x11] == 0x22,
           "both synthetic ROMs initialize and exchange the ten-byte seed block");
    expect(hostRam[0x13] == 0x33 && clientRam[0x13] == 0x33,
           "both peers finish the complete 512-byte party-like payload");
    expect(hostRam[0x12] == 0x55 && clientRam[0x12] == 0x55,
           "both synthetic ROMs finish the battle without action protocol errors");
    expect(normalRun.serialEdges == (10u + 512u + expected.turns) * 8u,
           "seed block, 512-byte payload and per-turn move bytes retain serial order");
    for (size_t index = 0; index < 512; ++index) {
        const uint8_t expectedHostPayload =
            static_cast<uint8_t>(37 * (index & 0xFF) + 0x51);
        const uint8_t expectedClientPayload =
            static_cast<uint8_t>(37 * (index & 0xFF) + 0x93);
        expect(hostRam[0x300 + index] == expectedHostPayload &&
                   clientRam[0x300 + index] == expectedClientPayload,
               "host and client prepare distinct 512-byte payloads");
        expect(hostRam[0x500 + index] == expectedClientPayload &&
                   clientRam[0x500 + index] == expectedHostPayload,
               "both peers save all 512 received bytes intact and in order");
    }

    const auto checkPeer = [&](const uint8_t *ram, const std::vector<uint8_t> &receivedActions) {
        expect(std::equal(expected.initialSeeds.begin(), expected.initialSeeds.end(), ram + 0x80),
               "SRAM preserves the exact ten-byte RNG table immediately after link exchange");
        expect(std::equal(expected.seeds.begin(), expected.seeds.end(), ram + 0x20),
               "SRAM contains the nine evolving shared seeds and unused tenth seed");
        expect(ram[0x30] == expected.rngIndex && ram[0x31] == expected.playerOneHp &&
                   ram[0x32] == expected.playerTwoHp && ram[0x33] == expected.turns &&
                   ram[0x34] == expected.hits && ram[0x35] == expected.criticalHits &&
                   ram[0x36] == expected.totalDamage && ram[0x38] == expected.misses &&
                   ram[0x3A] == 0,
               "SRAM battle HP, turns, hit, critical and damage counters match the reference");
        const size_t traceCountOffset = POKEMON_TEST_TRACE_COUNT - 0xA000;
        if (ram[traceCountOffset] != expected.trace.size())
            std::cerr << "synthetic battle RNG count: ROM="
                      << static_cast<unsigned>(ram[traceCountOffset])
                      << " reference=" << expected.trace.size() << '\n';
        expect(ram[traceCountOffset] == expected.trace.size(),
               "SRAM random-call count matches the shared PRNG reference");
        expect(std::equal(expected.trace.begin(), expected.trace.end(), ram + POKEMON_TEST_TRACE - 0xA000),
               "SRAM random trace follows the nine-value shared PRNG table");
        expect(std::equal(receivedActions.begin(), receivedActions.end(), ram + POKEMON_TEST_ACTION_LOG - 0xA000),
               "SRAM action transcript matches the received peer move bytes");
    };
    checkPeer(hostRam, expected.actionsReceivedByHost);
    checkPeer(clientRam, expected.actionsReceivedByClient);

    const auto driftRun = runPair(true, false, false, 100'000);
    const auto *driftHostRam = driftRun.hostSram.data();
    const auto *driftClientRam = driftRun.clientSram.data();
    const size_t traceCountOffset = POKEMON_TEST_TRACE_COUNT - 0xA000;
    const size_t traceOffset = POKEMON_TEST_TRACE - 0xA000;
    const bool driftTraceDiffers =
        driftHostRam[traceCountOffset] != driftClientRam[traceCountOffset] ||
        !std::equal(driftHostRam + traceOffset,
                    driftHostRam + traceOffset + driftHostRam[traceCountOffset],
                    driftClientRam + traceOffset);
    expect(driftHostRam[0x12] != 0x55 || driftClientRam[0x12] != 0x55 ||
               driftHostRam[0x31] != driftClientRam[0x31] ||
               driftHostRam[0x32] != driftClientRam[0x32] ||
               driftHostRam[0x33] != driftClientRam[0x33] || driftTraceDiffers,
           "SRAM checkpoints expose battle divergence after one peer consumes an extra RNG value");

    const auto corruptRun = runPair(false, true, false, 20'000, true);
    if (corruptRun.hostSram[0x11] != 0x22 || corruptRun.clientSram[0x11] != 0x22 ||
        std::equal(corruptRun.hostSram.begin() + 0x80,
                   corruptRun.hostSram.begin() + 0x8A,
                   corruptRun.clientSram.begin() + 0x80))
        std::cerr << "corrupt snapshot markers=" << static_cast<unsigned>(corruptRun.hostSram[0x11])
                  << "," << static_cast<unsigned>(corruptRun.clientSram[0x11])
                  << " seed[3]=" << static_cast<unsigned>(corruptRun.hostSram[0x83])
                  << "," << static_cast<unsigned>(corruptRun.clientSram[0x83]) << '\n';
    expect(corruptRun.hostSram[0x11] == 0x22 && corruptRun.clientSram[0x11] == 0x22 &&
               !std::equal(corruptRun.hostSram.begin() + 0x80,
                           corruptRun.hostSram.begin() + 0x8A,
                           corruptRun.clientSram.begin() + 0x80),
           "post-exchange SRAM snapshots detect a single corrupted shared seed byte");
}

void testMbc1MulticartCpuAndSnapshots() {
    auto rom = Test::makeMbc1MulticartRom();
    std::vector<uint8_t> program;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) { program.insert(program.end(), bytes); };
    emit({0xF3, 0x31, 0xFE, 0xFF, 0x3E, 0x0A, 0xEA, 0, 0});
    emit({0x3E, 1, 0xEA, 0, 0x60, 0x3E, 3, 0xEA, 0, 0x40});
    const size_t loop = 0x150 + program.size();
    for (const auto value : {0x10, 0, 0x1F}) {
        emit({0x3E, static_cast<uint8_t>(value), 0xEA, 0, 0x20, 0xFA, 0, 0x40});
        emit({0xEA, static_cast<uint8_t>(value == 0x10 ? 0x30 : value == 0 ? 0x31 : 0x32), 0xA0});
    }
    emit({0x21, 0x10, 0xA0, 0x34, 0xC3, static_cast<uint8_t>(loop), static_cast<uint8_t>(loop >> 8)});
    for (unsigned game = 0; game < 4; ++game)
        std::copy(program.begin(), program.end(), rom.begin() + game * 0x40000 + 0x150);
    for (auto model : {HardwareModel::Dmg, HardwareModel::CgbE}) {
        GameBoySystem original(rom, model), restored(rom, model);
        const bool multicart = Test::isSyntheticMbc1Multicart(rom);
        Test::GameBoySystemTestAccess::setMbc1Multicart(original, multicart);
        Test::GameBoySystemTestAccess::setMbc1Multicart(restored, multicart);
        auto *stable = restored.getSaveRamData();
        for (unsigned checkpoint = 0; checkpoint < 24; ++checkpoint) {
            for (unsigned cycle = 0; cycle < 17; ++cycle)
                expect(original.stepOneCycle().success, "multicart CPU crosses subgame bank changes");
            const auto state = original.saveState();
            restored.loadState(state.data(), state.size());
            expect(restored.getSaveRamData() == stable, "multicart snapshot preserves SRAM pointer");
            for (unsigned cycle = 0; cycle < 11; ++cycle)
                expect(original.stepOneCycle().success && restored.stepOneCycle().success,
                       "multicart CPU executes remapped lower window after restore");
            expect(original.takeAudioSamples() == restored.takeAudioSamples() && original.saveState() == restored.saveState(),
                   "MBC1M wiring reconstructed from ROM replays exact PCM and full state");
        }
        const auto *ram = original.getSaveRamData();
        expect(ram[0x30] == 0x30 && ram[0x31] == 0x31 && ram[0x32] == 0x3F,
               "guest observes MBC1M bit-four masking, zero translation and selected subgame banks");
        const auto progress = ram[0x10];
        auto battery = std::make_shared<std::vector<uint8_t>>(*original.getSaveRamStorage());
        GameBoySystem fresh(rom, model, battery);
        Test::GameBoySystemTestAccess::setMbc1Multicart(fresh, multicart);
        for (unsigned cycle = 0; cycle < 200; ++cycle)
            expect(fresh.stepOneCycle().success, "fresh multicart content resumes imported SRAM");
        expect(fresh.getSaveRamData()[0x10] > progress && fresh.getSaveRamData()[0x31] == 0x31,
               "multicart guest reads and advances imported battery progress on both hardware models");
    }
}

void testCpuIduCorruptsOamAndReplays() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 1;
    rom[0x143] = 0x80; rom[0x147] = 3; rom[0x149] = 2;
    size_t at = 0x150;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) { for (auto b : bytes) rom[at++] = b; };
    emit({0xF3, 0xAF, 0xE0, 0x40, 0x3E, 0x0A, 0xEA, 0, 0});
    emit({0x21, 0, 0xFE, 0x06, 160, 0x3E, 0x51});
    emit({0x22, 0xC6, 37, 0x05, 0x20, 0xFA}); // Fill OAM while LCD off.
    emit({0x01, 0xFF, 0xFE, 0x3E, 0x91, 0xE0, 0x40});
    emit({0xF0, 0x44, 0xFE, 1, 0x20, 0xFA}); // Reach mode 2 on line 1.
    emit({0x03}); const uint16_t afterInc = at;
    emit({0xF0, 0x44, 0xFE, 144, 0x20, 0xFA, 0xAF, 0xE0, 0x40});
    emit({0x21, 0, 0xFE, 0x11, 0, 0xA1, 0x06, 160});
    emit({0x2A, 0x12, 0x13, 0x05, 0x20, 0xFA, 0x18, 0xFE}); // Copy OAM to SRAM after scan.
    for (auto model : {HardwareModel::Dmg, HardwareModel::CgbE}) {
        GameBoySystem original(rom, model), restored(rom, model);
        unsigned cycles = 0;
        for (; cycles < 5000 && original.getCpuRegisters().getRefPC() != afterInc; ++cycles)
            expect(original.stepOneCycle().success, "CPU OAM probe fills data and fetches mode-two INC BC");
        expect(cycles < 5000 && original.getCpuRegisters().getRefBC() == 0xFEFF,
               "OAM probe snapshots before the IDU update crosses FEFF to FF00");
        const auto before = original.saveState(); restored.loadState(before.data(), before.size());
        for (unsigned n = 0; n < 25000; ++n)
            expect(original.stepOneCycle().success && restored.stepOneCycle().success,
                   "OAM probe runs IDU corruption and copies observable bytes through CPU");
        const auto *ram = original.getSaveRamData();
        unsigned changedRows = 0;
        for (unsigned row = 0; row < 20; ++row) {
            bool changed = false;
            for (unsigned byte = 0; byte < 8; ++byte)
                changed |= ram[0x100 + row * 8 + byte] != static_cast<uint8_t>((row * 8 + byte) * 37 + 0x51);
            if (changed) {
                ++changedRows;
                expect(row > 0 && row < 19, "CPU IDU corruption affects one visible scan row, not protected first objects");
                for (unsigned byte = 2; byte < 8; ++byte)
                    expect(ram[0x100 + row * 8 + byte] == static_cast<uint8_t>(((row - 1) * 8 + byte) * 37 + 0x51),
                           "CPU IDU write corruption copies the preceding row's last three words");
            }
        }
        expect(changedRows == (model == HardwareModel::Dmg ? 1u : 0u),
               "internal INC BC causes DMG OAM corruption while native CGB remains intact");
        expect(original.takeAudioSamples() == restored.takeAudioSamples() && original.saveState() == restored.saveState(),
               "pre-IDU snapshot reproduces exact OAM, CPU observation, PCM and complete state");
    }
}

}

int main(int argc, char **argv) {
    if (argc != 1) {
        if (argc != 3 || std::string(argv[1]) != "--generate-pokemon-link-probes") {
            std::cerr << "Usage: game_boy_system_test [--generate-pokemon-link-probes OUTPUT_DIR]\n";
            return EXIT_FAILURE;
        }
        const std::filesystem::path outputDirectory(argv[2]);
        try {
            std::filesystem::create_directories(outputDirectory);
            const auto writeBinary = [](const std::filesystem::path &path,
                                        const std::vector<uint8_t> &contents) {
                std::ofstream output(path, std::ios::binary);
                if (!output)
                    throw std::runtime_error("cannot open output file: " + path.string());
                output.write(reinterpret_cast<const char *>(contents.data()),
                             static_cast<std::streamsize>(contents.size()));
                if (!output)
                    throw std::runtime_error("failed to write output file: " + path.string());
            };
            writeBinary(outputDirectory / "host.gb", makePokemonBattleProbeRom(true));
            writeBinary(outputDirectory / "client.gb", makePokemonBattleProbeRom(false));
            std::ofstream notes(outputDirectory / "README.txt");
            if (!notes)
                throw std::runtime_error("cannot open generated ROM notes file");
            notes << "Original LotusGB serial test ROMs; no commercial code, art, or game data.\n"
                     "Load host.gb on the internal-clock side and client.gb on the external-clock side.\n"
                     "The ROMs exchange a fixed 10-byte seed block, then a 512-byte patterned payload,\n"
                     "then up to eight per-turn move bytes before running a small deterministic battle.\n"
                     "SRAM checkpoints: A010=start, A011=post-seed snapshot, A013=payload complete,\n"
                     "A012=55 means the battle completed; A080-A089=seed snapshot;\n"
                     "A100 onward=BattleRandom trace (count at A0F0); A200 onward=received actions;\n"
                     "A300-A4FF=outgoing payload; A500-A6FF=received peer payload. SRAM is saved by the frontend when the content closes.\n"
                     "This synthetic battle models shared-table consumption and is not a full game ROM.\n";
            if (!notes)
                throw std::runtime_error("failed to write generated ROM notes file");
        } catch (const std::exception &error) {
            std::cerr << "Could not generate synthetic link ROMs: " << error.what() << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "Generated original serial test ROM pair in " << outputDirectory << '\n';
        return EXIT_SUCCESS;
    }

    testPpuAccessWindowsSupportSafeCpuUpdates();
    testLycZeroHandlerRunsBeforeVblankEnds();
    testVblankInterruptWakesHaltAndRunsHandler();
    testDisconnectedSerialWakesHaltAndRunsHandler();
    testSerialDiagnosticStateCapturesCpuAndInterruptRegisters();
    testSerialClockIsIndependentOfDividerWrites();
    testCpuRunsHramDmaRoutineAndReturnsToRom();
    testPostBootInterruptsWaitForEi();
    testCpuFetchesAndExecutesFromCartridgeThroughSystemBus();
    testDmgBootRomOverlaysAndUnmapsBeforeCartridgeExecution();
    testEchoRamReadsAndWritesWorkRam();
    testTimerInterruptWakesHaltAndRunsHandler();
    testSystemTimerClocksApuFrameSequencer();
    testSystemBusMapsWaveRamToApu();
    testGameBoySystemAcceptsButtonInput();
    testPpuAdvancesAndExposesScanlineThroughSystemBus();
    testGameBoySystemExposesRenderedFrameBuffer();
    testDmgPostBootStartupAndFrameStepping();
    testFrameSteppingReturnsWhenCpuStops();
    testVblankSteppingUsesFrameEventsAndSurvivesRestore();
    testVblankSteppingBoundsDisabledStoppedAndLockedSystems();
    testAudioCountsActualCyclesAcrossLcdRestartStepping();
    testPresentationReasonsSeparateHostRepeatFromHardwareVblank();
    testDmgSystemDoesNotExposeCgbBankRegisters();
    testSelectedJoypadLineWakesStopWithoutInterruptAcceptance();
    testStopRequiresFreshFallingJoypadEdgeWhenLineWasAlreadyLow();
    testTwoSystemsSerialExchangeAndPairedRestore();
    testPokemonBattleProbeRomSynchronizesSeedsAndBattleState();
    testMbc1MulticartCpuAndSnapshots();
    testCpuIduCorruptsOamAndReplays();
    std::cout << "Headless system tests passed\n";
}
