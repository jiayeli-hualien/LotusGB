#include "game_boy_system.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

bool hasPassed(LOTUSGB::Reg &registers) {
    return registers.getRefB() == 3 &&
           registers.getRefC() == 5 && registers.getRefD() == 8 &&
           registers.getRefE() == 13 && registers.getRefH() == 21 &&
           registers.getRefL() == 34;
}

bool hasFailed(LOTUSGB::Reg &registers) {
    return registers.getRefB() == 0x42 && registers.getRefC() == 0x42 &&
           registers.getRefD() == 0x42 && registers.getRefE() == 0x42 &&
           registers.getRefH() == 0x42 && registers.getRefL() == 0x42;
}

void printRegisters(LOTUSGB::Reg &registers) {
    std::cerr << "A=$" << std::hex << static_cast<unsigned>(registers.getRefA())
              << " B=$" << static_cast<unsigned>(registers.getRefB())
              << " C=$" << static_cast<unsigned>(registers.getRefC())
              << " D=$" << static_cast<unsigned>(registers.getRefD())
              << " E=$" << static_cast<unsigned>(registers.getRefE())
              << " H=$" << static_cast<unsigned>(registers.getRefH())
              << " L=$" << static_cast<unsigned>(registers.getRefL())
              << " PC=$" << registers.getRefPC() << std::dec;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2 &&
        (argc != 4 || std::string(argv[2]) != "--boot-rom")) {
        std::cerr << "Usage: mooneye_rom_runner <test-rom.gb> "
                     "[--boot-rom <dmg-boot-rom.bin>]\n";
        return 2;
    }

    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::cerr << "Could not open ROM: " << argv[1] << '\n';
        return 2;
    }

    std::vector<uint8_t> rom(std::istreambuf_iterator<char>(file), {});
    if (rom.empty()) {
        std::cerr << "ROM is empty: " << argv[1] << '\n';
        return 2;
    }

    std::vector<uint8_t> bootRom;
    if (argc == 4) {
        std::ifstream bootRomFile(argv[3], std::ios::binary);
        if (!bootRomFile) {
            std::cerr << "Could not open DMG boot ROM: " << argv[3] << '\n';
            return 2;
        }
        bootRom.assign(std::istreambuf_iterator<char>(bootRomFile), {});
        if (bootRom.size() != 0x100) {
            std::cerr << "DMG boot ROM must be exactly 256 bytes: " << argv[3]
                      << '\n';
            return 2;
        }
    }

    // Keep the ROM image to recognize the protocol breakpoint. The curated
    // tests report from fixed ROM code, not bank-switched RAM/HRAM code.
    LOTUSGB::GameBoySystem system(rom, nullptr, nullptr, std::move(bootRom));
    constexpr uint64_t MAX_M_CYCLES = 10'000'000;
    for (uint64_t cycle = 0; cycle < MAX_M_CYCLES; ++cycle) {
        const auto result = system.stepOneCycle();
        if (!result.success) {
            std::cerr << "CPU step failed after " << cycle << " M-cycles\n";
            return 2;
        }

        LOTUSGB::Reg registers = system.getCpuRegisters();
        // Register patterns alone can occur during normal test execution.
        // Mooneye requires LD B,B ($40) as a breakpoint as well. PC points
        // past the prefetched opcode in this CPU, so inspect PC-1 only when
        // a complete instruction's next opcode has been fetched.
        const uint16_t opcodeAddress = static_cast<uint16_t>(registers.getRefPC() - 1);
        if (!result.fetchedNewInst || opcodeAddress >= 0x8000 ||
            opcodeAddress >= rom.size() || rom[opcodeAddress] != 0x40)
            continue;
        if (hasPassed(registers)) {
            std::cout << "PASS after " << cycle + 1 << " M-cycles\n";
            return 0;
        }
        if (hasFailed(registers)) {
            std::cerr << "FAIL signature after " << cycle + 1 << " M-cycles: ";
            printRegisters(registers);
            std::cerr << '\n';
            return 1;
        }
    }

    LOTUSGB::Reg registers = system.getCpuRegisters();
    std::cerr << "TIMEOUT after " << MAX_M_CYCLES << " M-cycles: ";
    printRegisters(registers);
    std::cerr << '\n';
    return 1;
}
