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

    GameBoySystem system(std::move(rom));

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

    GameBoySystem system(std::move(rom));
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

void testGameBoySystemAcceptsButtonInput() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x3E, 0x20,       // LD A, $20
        0xE0, 0x00,       // LDH ($FF00), A (select directions)
        0xFA, 0x00, 0xFF, // LD A, ($FF00)
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        rom[i] = program[i];

    GameBoySystem system(std::move(rom));
    system.setButtonState(JoypadButton::Right, true);
    for (unsigned step = 0; step < 16; ++step)
        expect(system.stepOneCycle().success, "button input program advances successfully");

    expect(system.getCpuRegisters().getRefA() == 0xEE,
           "button state reaches the CPU through the joypad register on the system bus");
}

}

int main() {
    testCpuFetchesAndExecutesFromCartridgeThroughSystemBus();
    testTimerInterruptWakesHaltAndRunsHandler();
    testGameBoySystemAcceptsButtonInput();
    std::cout << "Headless system tests passed\n";
}
