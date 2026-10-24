#include "game_boy_system.hpp"

#include <fstream>
#include <filesystem>
#include <exception>
#include <iostream>
#include <iterator>
#include <vector>

// Selected visual ROMs report from fixed ROM0 code with an LD B,B marker.
// Stop at the marker, not at an arbitrary host frame count.
int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: ppu_frame_runner ROM.gb OUTPUT.shades\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "Cannot open ROM\n";
        return 2;
    }
    const std::vector<uint8_t> rom(std::istreambuf_iterator<char>(input), {});
    try {
        if (std::filesystem::exists(argv[2]) &&
            std::filesystem::equivalent(argv[1], argv[2])) {
            std::cerr << "Output must not overwrite input ROM\n";
            return 2;
        }
        LOTUSGB::GameBoySystem system(rom);
        constexpr uint64_t MAX_M_CYCLES = 10'000'000;
        for (uint64_t cycle = 0; cycle < MAX_M_CYCLES; ++cycle) {
            const auto step = system.stepOneCycle();
            if (!step.success || step.tCyclesElapsed == 0) {
                std::cerr << "CPU failed or stopped before visual breakpoint\n";
                return 1;
            }
            // Discard audio periodically; this tool only captures shade indices.
            if (cycle % 17556 == 0)
                system.takeAudioSamples();
            auto registers = system.getCpuRegisters();
            const uint16_t address = static_cast<uint16_t>(registers.getRefPC() - 1);
            if (!step.fetchedNewInst || address >= 0x4000 ||
                address >= rom.size() || rom[address] != 0x40)
                continue;
            const auto &frame = system.getFrameBuffer();
            std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char *>(frame.data()), frame.size());
            output.close();
            if (!output) {
                std::cerr << "Cannot write captured frame\n";
                return 2;
            }
            std::cout << "CAPTURE pc=" << address << " mcycles=" << cycle + 1 << '\n';
            return 0;
        }
        std::cerr << "TIMEOUT before visual breakpoint\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
