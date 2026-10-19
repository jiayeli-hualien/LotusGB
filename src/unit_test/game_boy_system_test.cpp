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

}

int main() {
    testCpuFetchesAndExecutesFromCartridgeThroughSystemBus();
    std::cout << "Headless system tests passed\n";
}
