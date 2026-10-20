#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "game_boy_system.hpp"

#ifndef LOTUSGB_ACID2_ASSET_DIR
#error LOTUSGB_ACID2_ASSET_DIR must name the dmg-acid2 test assets directory
#endif

namespace {

using namespace LOTUSGB;

std::vector<uint8_t> readBinary(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "FAIL: cannot open " << path << '\n';
        std::exit(EXIT_FAILURE);
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>());
}

void testDmgAcid2MatchesReferenceFrame() {
    const std::string assetDir = LOTUSGB_ACID2_ASSET_DIR;
    std::vector<uint8_t> rom = readBinary(assetDir + "/dmg-acid2.gb");
    const std::vector<uint8_t> expected =
        readBinary(assetDir + "/reference-shades.bin");

    if (rom.size() != 0x8000) {
        std::cerr << "FAIL: dmg-acid2 ROM must be 32 KiB\n";
        std::exit(EXIT_FAILURE);
    }
    if (expected.size() != DmgPpu::FRAME_WIDTH * DmgPpu::FRAME_HEIGHT) {
        std::cerr << "FAIL: dmg-acid2 reference must contain one 160x144 frame\n";
        std::exit(EXIT_FAILURE);
    }

    GameBoySystem system(std::move(rom));
    constexpr unsigned T_CYCLES_PER_FRAME = 456 * 154;
    constexpr unsigned M_CYCLES_PER_FRAME =
        T_CYCLES_PER_FRAME / T_CYCLES_PER_M_CYCLE;
    constexpr unsigned FRAMES_TO_RUN = 10;
    for (unsigned i = 0; i < FRAMES_TO_RUN * M_CYCLES_PER_FRAME; ++i) {
        const auto result = system.stepOneCycle();
        if (!result.success) {
            std::cerr << "FAIL: dmg-acid2 failed at M-cycle " << i << '\n';
            std::exit(EXIT_FAILURE);
        }
    }

    const auto &actual = system.getFrameBuffer();
    unsigned mismatchCount = 0;
    size_t firstMismatch = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (actual[i] != expected[i]) {
            if (mismatchCount == 0)
                firstMismatch = i;
            ++mismatchCount;
        }
    }

    if (mismatchCount != 0) {
        std::cerr << "FAIL: dmg-acid2 frame differs at " << mismatchCount
                  << " / " << expected.size() << " pixels; first mismatch at ("
                  << firstMismatch % DmgPpu::FRAME_WIDTH << ','
                  << firstMismatch / DmgPpu::FRAME_WIDTH << "): got shade "
                  << static_cast<unsigned>(actual[firstMismatch])
                  << ", expected "
                  << static_cast<unsigned>(expected[firstMismatch]) << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}

int main() {
    testDmgAcid2MatchesReferenceFrame();
    std::cout << "dmg-acid2 visual regression passed\n";
}
