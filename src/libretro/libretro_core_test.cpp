#include "libretro.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#ifndef LOTUSGB_LIBRETRO_TEST_ASSET_DIR
#error LOTUSGB_LIBRETRO_TEST_ASSET_DIR must name the test ROM assets directory
#endif

namespace {

constexpr unsigned WIDTH = 160;
constexpr unsigned HEIGHT = 144;

unsigned inputPollCount = 0;
unsigned videoRefreshCount = 0;
unsigned observedPitch = 0;
bool upPressed = false;
std::array<uint32_t, WIDTH * HEIGHT> observedFrame{};

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::vector<uint8_t> readBinary(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    expect(static_cast<bool>(file), "test asset opens");
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>());
}

bool environmentCallback(unsigned command, void *data) {
    if (command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) {
        const unsigned pixelFormat = *static_cast<unsigned *>(data);
        expect(pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888,
               "core requests XRGB8888 output");
        return true;
    }
    if (command == RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS)
        return true;
    return false;
}

void inputPollCallback() {
    ++inputPollCount;
}

int16_t inputStateCallback(unsigned port, unsigned device, unsigned index,
                           unsigned id) {
    if (port == 0 && device == RETRO_DEVICE_JOYPAD && index == 0 &&
        id == RETRO_DEVICE_ID_JOYPAD_UP && upPressed)
        return 1;
    return 0;
}

void videoRefreshCallback(const void *data, unsigned width, unsigned height,
                          size_t pitch) {
    ++videoRefreshCount;
    expect(data != nullptr, "core returns a video frame");
    expect(width == WIDTH && height == HEIGHT,
           "core returns native 160x144 dimensions");
    expect(pitch == WIDTH * sizeof(uint32_t),
           "core reports the XRGB8888 pitch in bytes");
    observedPitch = static_cast<unsigned>(pitch);
    const auto *pixels = static_cast<const uint8_t *>(data);
    std::memcpy(observedFrame.data(), pixels, sizeof(observedFrame));
}

void testLibretroLoadsAcid2AndPublishesReferenceFrame() {
    const std::string assetDir = LOTUSGB_LIBRETRO_TEST_ASSET_DIR;
    const std::vector<uint8_t> rom = readBinary(assetDir + "/dmg-acid2/dmg-acid2.gb");
    const std::vector<uint8_t> expected =
        readBinary(assetDir + "/dmg-acid2/reference-shades.bin");

    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    expect(!retro_load_game(nullptr), "core rejects missing game content");

    std::vector<uint8_t> unsupported = rom;
    unsupported[0x0147] = 0x01; // MBC1 is outside the current system scope.
    retro_game_info unsupportedGame{};
    unsupportedGame.data = unsupported.data();
    unsupportedGame.size = unsupported.size();
    expect(!retro_load_game(&unsupportedGame),
           "core rejects cartridge mappers it does not implement");

    expect(retro_load_game(&game), "core loads a ROM-only homebrew cartridge");
    for (unsigned frame = 0; frame < 10; ++frame)
        retro_run();

    expect(inputPollCount == 10,
           "core polls frontend input once per video frame");
    expect(videoRefreshCount == 10,
           "core publishes exactly one video frame per retro_run call");
    expect(observedPitch == WIDTH * sizeof(uint32_t),
           "core keeps a stable XRGB8888 video pitch");
    expect(expected.size() == observedFrame.size(),
           "reference frame has the expected pixel count");

    for (size_t i = 0; i < expected.size(); ++i) {
        const uint8_t gray = static_cast<uint8_t>(255 - expected[i] * 85);
        const uint32_t expectedPixel = (static_cast<uint32_t>(gray) << 16) |
                                       (static_cast<uint32_t>(gray) << 8) | gray;
        if (observedFrame[i] != expectedPixel) {
            std::cerr << "FAIL: libretro frame differs at pixel " << i
                      << ": got 0x" << std::hex << observedFrame[i]
                      << ", expected 0x" << expectedPixel << std::dec << '\n';
            std::exit(EXIT_FAILURE);
        }
    }
}

std::array<uint32_t, WIDTH * HEIGHT> runPongWithUpButton(
    const std::vector<uint8_t> &rom, bool holdUp) {
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    expect(retro_load_game(&game), "core loads the ROM-only Pong game");

    inputPollCount = 0;
    videoRefreshCount = 0;
    upPressed = holdUp;
    constexpr unsigned FRAMES = 60;
    for (unsigned frame = 0; frame < FRAMES; ++frame)
        retro_run();

    expect(inputPollCount == FRAMES,
           "Pong run polls input once for every frame");
    expect(videoRefreshCount == FRAMES,
           "Pong run publishes one frame for every retro_run call");
    const auto result = observedFrame;
    retro_unload_game();
    return result;
}

void testLibretroInputChangesPongGameplayFrame() {
    const std::string assetDir = LOTUSGB_LIBRETRO_TEST_ASSET_DIR;
    const std::vector<uint8_t> rom = readBinary(assetDir + "/gb-pong/gb-pong.gb");
    const auto neutralFrame = runPongWithUpButton(rom, false);
    const auto upFrame = runPongWithUpButton(rom, true);

    size_t differingPixels = 0;
    for (size_t i = 0; i < neutralFrame.size(); ++i)
        differingPixels += neutralFrame[i] != upFrame[i];
    expect(differingPixels > 0,
           "holding D-Pad Up visibly moves the player's paddle in Pong");
}

void testLibretroReportsApiAndVideoTiming() {
    expect(retro_api_version() == RETRO_API_VERSION,
           "core reports the libretro API version");

    retro_system_info info{};
    retro_get_system_info(&info);
    expect(info.valid_extensions != nullptr &&
               std::string(info.valid_extensions) == "gb",
           "core advertises Game Boy ROM content");
    expect(!info.need_fullpath,
           "core accepts frontend-provided in-memory ROM data");

    retro_system_av_info avInfo{};
    retro_get_system_av_info(&avInfo);
    expect(avInfo.geometry.base_width == WIDTH &&
               avInfo.geometry.base_height == HEIGHT,
           "core reports native Game Boy video geometry");
    expect(avInfo.timing.fps > 59.7 && avInfo.timing.fps < 59.8,
           "core reports the DMG frame rate");
}

}

int main() {
    retro_set_environment(environmentCallback);
    retro_set_video_refresh(videoRefreshCallback);
    retro_set_input_poll(inputPollCallback);
    retro_set_input_state(inputStateCallback);
    retro_init();

    testLibretroReportsApiAndVideoTiming();
    testLibretroLoadsAcid2AndPublishesReferenceFrame();
    testLibretroInputChangesPongGameplayFrame();

    retro_unload_game();
    retro_deinit();
    std::cout << "Libretro adapter tests passed\n";
}
