#include "libretro.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "gba_boot_rom.hpp"

namespace {

bool videoReceived = false;
unsigned receivedWidth = 0;
unsigned receivedHeight = 0;
std::vector<uint32_t> receivedPixels;
enum retro_pixel_format selectedFormat = RETRO_PIXEL_FORMAT_0RGB1555;
bool acceptXrgb8888 = true;
uint16_t pressedJoypad = 0;

void inputPoll() {}

int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || index != 0 || id >= 16)
        return 0;
    return (pressedJoypad & (1u << id)) != 0 ? 1 : 0;
}

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

unsigned countColor(uint32_t color, unsigned x0 = 0, unsigned y0 = 0,
                    unsigned x1 = 240, unsigned y1 = 160) {
    unsigned count = 0;
    for (unsigned y = y0; y < y1; ++y)
        for (unsigned x = x0; x < x1; ++x)
            count += receivedPixels[y * 240 + x] == color;
    return count;
}

int leftPaddleTop() {
    for (unsigned y = 0; y < 160; ++y)
        for (unsigned x = 1; x < 9; ++x)
            if (receivedPixels[y * 240 + x] == 0x000000FF)
                return static_cast<int>(y);
    return -1;
}

std::pair<int, int> ballTopLeft() {
    int left = 240;
    int top = 160;
    for (unsigned y = 0; y < 160; ++y) {
        for (unsigned x = 0; x < 240; ++x) {
            if (receivedPixels[y * 240 + x] == 0x0000FF00) {
                left = std::min(left, static_cast<int>(x));
                top = std::min(top, static_cast<int>(y));
            }
        }
    }
    return {left, top};
}

unsigned scorePixels(unsigned x0) {
    return countColor(0x00FFFFFF, x0, 10, x0 + 16, 26);
}

void runPongGameplayAndCheck() {
    auto rom = readFile("../../tests/roms/gba-pong/pong.gba");
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    assert(retro_load_game(&game));

    std::pair<int, int> initialBall{};
    std::pair<int, int> beforePaddleReturn{};
    unsigned initialPlayerScore = 0;
    unsigned initialComputerScore = 0;
    for (unsigned frame = 0; frame <= 460; ++frame) {
        pressedJoypad = 0;
        if (frame == 2)
            pressedJoypad = 1u << RETRO_DEVICE_ID_JOYPAD_START;
        if (frame >= 130 && frame < 140)
            pressedJoypad = 1u << RETRO_DEVICE_ID_JOYPAD_UP;
        if (frame >= 141 && frame < 151)
            pressedJoypad = 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
        if (frame >= 185 && frame < 220)
            pressedJoypad = 1u << RETRO_DEVICE_ID_JOYPAD_UP;
        if (frame >= 400 && frame < 434)
            pressedJoypad = 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;

        videoReceived = false;
        retro_run();
        assert(videoReceived);
        assert(receivedWidth == 240 && receivedHeight == 160);

        if (frame == 1) {
            // The title and both menu choices occupy the middle of Mode 3.
            assert(countColor(0x00FFFFFF, 80, 64, 160, 72) > 0);
            assert(countColor(0x00FFFFFF, 72, 88, 160, 112) > 0);
            assert(countColor(0x000000FF) == 0);
        }
        if (frame == 4) {
            assert(countColor(0x000000FF) > 0);
            assert(countColor(0x0000FF00) == 64);
            assert(leftPaddleTop() == 68);
            initialBall = ballTopLeft();
            initialPlayerScore = scorePixels(52);
            initialComputerScore = scorePixels(172);
            assert(initialPlayerScore > 0 && initialComputerScore > 0);
        }
        if (frame == 125)
            assert(ballTopLeft() != initialBall);
        if (frame == 129)
            assert(leftPaddleTop() == 68);
        if (frame == 139)
            assert(leftPaddleTop() == 48);
        if (frame == 150)
            assert(leftPaddleTop() == 68);
        if (frame == 219)
            assert(leftPaddleTop() == 0);
        if (frame == 220)
            beforePaddleReturn = ballTopLeft();
        if (frame == 245) {
            // The human paddle catches the ball at the left edge and sends it
            // back across the screen.
            const auto returnedBall = ballTopLeft();
            assert(returnedBall.first > beforePaddleReturn.first);
            assert(returnedBall.first < 80);
        }
        if (frame == 433)
            assert(leftPaddleTop() == 68);
        if (frame == 454 || frame == 460) {
            // Moving away for the next return lets the CPU score once.
            assert(scorePixels(52) == initialPlayerScore);
            assert(scorePixels(172) != initialComputerScore);
        }
    }

    retro_unload_game();
}

} // namespace

int main() {
    retro_set_environment(environment);
    retro_set_video_refresh(videoRefresh);
    retro_set_input_poll(inputPoll);
    retro_set_input_state(inputState);
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
    runPongGameplayAndCheck();

    retro_deinit();

    acceptXrgb8888 = false;
    retro_init();
    assert(selectedFormat == RETRO_PIXEL_FORMAT_0RGB1555);
    runAndCheck(makeGbaThumbMode3BootRom());
    retro_unload_game();
    retro_deinit();
}
