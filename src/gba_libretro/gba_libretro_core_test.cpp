#include "libretro.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "gba_boot_rom.hpp"

namespace {

bool videoReceived = false;
unsigned receivedWidth = 0;
unsigned receivedHeight = 0;
std::vector<uint32_t> receivedPixels;
enum retro_pixel_format selectedFormat = RETRO_PIXEL_FORMAT_0RGB1555;
bool acceptXrgb8888 = true;

std::vector<uint8_t> readFile(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    assert(input.good());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>());
}

bool environment(unsigned command, void *data) {
    if (command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) {
        selectedFormat = *static_cast<retro_pixel_format *>(data);
        return selectedFormat == RETRO_PIXEL_FORMAT_XRGB8888
                   ? acceptXrgb8888
                   : selectedFormat == RETRO_PIXEL_FORMAT_0RGB1555;
    }
    return false;
}

void videoRefresh(const void *data, unsigned width, unsigned height, size_t pitch) {
    assert(data != nullptr);
    videoReceived = true;
    receivedWidth = width;
    receivedHeight = height;
    receivedPixels.clear();
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            if (selectedFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
                uint32_t pixel = 0;
                const size_t offset = static_cast<size_t>(y) * pitch + x * 4;
                pixel = static_cast<uint32_t>(bytes[offset]) |
                        (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                        (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                        (static_cast<uint32_t>(bytes[offset + 3]) << 24);
                receivedPixels.push_back(pixel);
            } else {
                const size_t offset = static_cast<size_t>(y) * pitch + x * 2;
                receivedPixels.push_back(static_cast<uint32_t>(bytes[offset]) |
                    (static_cast<uint32_t>(bytes[offset + 1]) << 8));
            }
        }
    }
}

} // namespace

int main() {
    retro_set_environment(environment);
    retro_set_video_refresh(videoRefresh);
    retro_init();

    retro_system_info systemInfo{};
    retro_get_system_info(&systemInfo);
    assert(systemInfo.library_name && std::string(systemInfo.library_name) == "LotusGBA");
    assert(systemInfo.valid_extensions && std::string(systemInfo.valid_extensions) == "gba");
    assert(!systemInfo.need_fullpath);

    retro_system_av_info avInfo{};
    retro_get_system_av_info(&avInfo);
    assert(avInfo.geometry.base_width == 240);
    assert(avInfo.geometry.base_height == 160);
    assert(std::fabs(avInfo.timing.fps - 59.7275) < 0.01);

    const auto runAndCheck = [](std::vector<uint8_t> rom) {
        retro_game_info game{};
        game.data = rom.data();
        game.size = rom.size();
        videoReceived = false;
        receivedPixels.clear();
        assert(retro_load_game(&game));
        retro_run();

        assert(videoReceived);
        assert(receivedWidth == 240 && receivedHeight == 160);
        assert(receivedPixels.size() == 240 * 160);
        if (selectedFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
            for (uint32_t pixel : receivedPixels)
                assert(pixel == 0x00FF0000);
        } else {
            for (uint32_t pixel : receivedPixels)
                assert(pixel == 0x7C00);
        }
    };
    runAndCheck(makeGbaMode3BootRom());
    retro_unload_game();
    runAndCheck(makeGbaThumbMode3BootRom());

    constexpr const char *letterA[] = {
        "..####..",
        ".##..##.",
        ".##..##.",
        ".######.",
        ".##..##.",
        ".##..##.",
        ".##..##.",
        "........",
    };
    const auto runInstructionSuiteAndCheck = [&](std::vector<uint8_t> rom) {
        retro_game_info game{};
        game.data = rom.data();
        game.size = rom.size();
        assert(retro_load_game(&game));
        for (unsigned frame = 0; frame < 5; ++frame) {
            videoReceived = false;
            retro_run();
            assert(videoReceived);
            assert(receivedWidth == 240 && receivedHeight == 160);
        }

        // Both upstream suites print "All tests passed" at (56, 76), while
        // their numbered failure report starts at a different position.
        assert(selectedFormat == RETRO_PIXEL_FORMAT_XRGB8888);
        for (unsigned y = 0; y < 8; ++y) {
            for (unsigned x = 0; x < 8; ++x) {
                const uint32_t expected = letterA[y][x] == '#'
                    ? 0x00000000
                    : 0x00FFFFFF;
                assert(receivedPixels[(76 + y) * 240 + 56 + x] == expected);
            }
        }
        retro_unload_game();
    };
    retro_unload_game();
    runInstructionSuiteAndCheck(
        readFile("../../tests/roms/gba-arm-tests/arm.gba"));
    runInstructionSuiteAndCheck(
        readFile("../../tests/roms/gba-thumb-tests/thumb.gba"));

    retro_deinit();

    acceptXrgb8888 = false;
    retro_init();
    assert(selectedFormat == RETRO_PIXEL_FORMAT_0RGB1555);
    runAndCheck(makeGbaThumbMode3BootRom());
    retro_unload_game();
    retro_deinit();
}
