#include "libretro.h"
#include "callback_exception_boundary.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <stdexcept>
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
bool frontendAcceptsXrgb8888 = true;
bool upPressed = false;
unsigned audioBatchCallCount = 0;
size_t audioFrameCount = 0;
size_t nonzeroAudioSampleCount = 0;
std::vector<size_t> audioBatchSizes;
std::vector<int16_t> latestAudio;
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

std::vector<std::string> logMessages;
std::vector<retro_log_level> logLevels;
std::string systemDirectory;

void frontendLog(retro_log_level level, const char *format, ...) {
    char buffer[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    logMessages.emplace_back(buffer);
    logLevels.push_back(level);
}

bool environmentCallback(unsigned command, void *data) {
    if (command == RETRO_ENVIRONMENT_GET_LOG_INTERFACE) {
        static_cast<retro_log_callback *>(data)->log = frontendLog;
        return true;
    }
    if (command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) {
        const unsigned pixelFormat = *static_cast<unsigned *>(data);
        expect(pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888,
               "core requests XRGB8888 output");
        return frontendAcceptsXrgb8888;
    }
    if (command == RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS)
        return true;
    if (command == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY) {
        if (systemDirectory.empty())
            return false;
        *static_cast<const char **>(data) = systemDirectory.c_str();
        return true;
    }
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
    const size_t bytesPerPixel = frontendAcceptsXrgb8888 ? sizeof(uint32_t)
                                                       : sizeof(uint16_t);
    expect(pitch == WIDTH * bytesPerPixel,
           "core reports the negotiated video pitch in bytes");
    observedPitch = static_cast<unsigned>(pitch);
    const auto *pixels = static_cast<const uint8_t *>(data);
    if (frontendAcceptsXrgb8888) {
        std::memcpy(observedFrame.data(), pixels, sizeof(observedFrame));
    } else {
        for (size_t i = 0; i < observedFrame.size(); ++i) {
            uint16_t pixel = 0;
            std::memcpy(&pixel, pixels + i * sizeof(pixel), sizeof(pixel));
            const auto expand = [](unsigned channel) {
                return (channel << 3) | (channel >> 2);
            };
            observedFrame[i] = (expand((pixel >> 10) & 31) << 16) |
                              (expand((pixel >> 5) & 31) << 8) |
                               expand(pixel & 31);
        }
    }
}

size_t audioSampleBatchCallback(const int16_t *data, size_t frames) {
    ++audioBatchCallCount;
    latestAudio.assign(data, data + frames * 2);
    audioFrameCount += frames;
    audioBatchSizes.push_back(frames);
    for (size_t i = 0; i < frames * 2; ++i)
        nonzeroAudioSampleCount += data[i] != 0;
    return frames;
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
    unsupported[0x0147] = 0x22; // MBC7 is not implemented yet.
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

size_t countDarkPixels(const std::array<uint32_t, WIDTH * HEIGHT> &frame,
                       unsigned xBegin, unsigned xEnd,
                       unsigned yBegin, unsigned yEnd) {
    size_t count = 0;
    for (unsigned y = yBegin; y < yEnd; ++y) {
        for (unsigned x = xBegin; x < xEnd; ++x) {
            const uint32_t pixel = frame[y * WIDTH + x];
            const uint8_t red = static_cast<uint8_t>((pixel >> 16) & 0xFF);
            const uint8_t green = static_cast<uint8_t>((pixel >> 8) & 0xFF);
            const uint8_t blue = static_cast<uint8_t>(pixel & 0xFF);
            if (red < 250 || green < 250 || blue < 250)
                ++count;
        }
    }
    return count;
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
           "holding D-Pad Up changes the Pong gameplay frame");
    expect(countDarkPixels(neutralFrame, 4, 12, 50, 100) > 100 &&
               countDarkPixels(neutralFrame, 4, 12, 10, 40) < 16,
           "neutral Pong leaves the player's paddle in the middle of the screen");
    expect(countDarkPixels(upFrame, 4, 12, 10, 40) > 100 &&
               countDarkPixels(upFrame, 4, 12, 50, 100) < 16,
           "holding Up moves the player's paddle from the middle to the top");
}

void testLibretroPublishesPulseChannelAudio() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x3E, 0xF0, 0xE0, 0x12, // NR12: enable channel 1 DAC at volume 15
        0x3E, 0x80, 0xE0, 0x11, // NR11: 50% duty
        0x3E, 0x77, 0xE0, 0x24, // NR50: full output volume
        0x3E, 0x11, 0xE0, 0x25, // NR51: route channel 1 left and right
        0x3E, 0x00, 0xE0, 0x13, // NR13: frequency low
        0x3E, 0x86, 0xE0, 0x14, // NR14: trigger channel 1
        0x18, 0xFE,             // loop forever
    };
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x100);
    rom[0x147] = 0x00; // ROM-only cartridge

    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    expect(retro_load_game(&game), "core loads the pulse-channel test ROM");

    audioBatchCallCount = 0;
    audioFrameCount = 0;
    nonzeroAudioSampleCount = 0;
    audioBatchSizes.clear();
    retro_run();
    retro_run();

    expect(audioBatchCallCount == 2,
           "core submits one audio batch per video frame");
    expect(audioFrameCount > 0,
           "core submits audio frames to the frontend");
    expect(audioBatchSizes.size() == 2 && audioBatchSizes[0] == 803 &&
               audioBatchSizes[1] == 804 && audioFrameCount == 1607,
           "audio sample batches follow the 48 kHz clock across video frames");
    expect(nonzeroAudioSampleCount > 0,
           "channel 1 ROM output reaches the frontend as non-silent PCM");
    retro_unload_game();
}

void testLibretroLoadsMaximumMbc5RomSize() {
    std::vector<uint8_t> rom(0x800000, 0x00);
    rom[0x0147] = 0x19; // MBC5
    rom[0x0148] = 0x08; // 8 MiB ROM
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();

    expect(retro_load_game(&game), "core loads an 8 MiB MBC5 ROM");
    retro_unload_game();

    rom.resize(0x804000);
    game.size = rom.size();
    expect(!retro_load_game(&game), "core rejects ROMs larger than 8 MiB");
}

void testLibretroExposesMbc1BatteryRam() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x3E, 0x0A,             // LD A, $0A
        0xEA, 0x00, 0x00,       // LD ($0000), A (enable cartridge RAM)
        0x3E, 0x5A,             // LD A, $5A
        0xEA, 0x00, 0xA0,       // LD ($A000), A
        0x18, 0xFE,             // loop forever
    };
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x0100);
    rom[0x0147] = 0x03; // MBC1 + RAM + battery
    rom[0x0148] = 0x00; // 32 KiB ROM
    rom[0x0149] = 0x02; // 8 KiB external RAM

    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    expect(retro_load_game(&game),
           "core loads an original-header MBC1 battery cartridge");
    for (unsigned frame = 0; frame < 2; ++frame)
        retro_run();

    expect(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0x2000,
           "libretro reports the cartridge's declared external RAM size");
    auto *saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(saveRam != nullptr && saveRam[0] == 0x5A,
           "CPU writes to MBC1 RAM reach the libretro save RAM buffer");
    uint8_t *stableSaveRamPointer = saveRam;
    saveRam[0] = 0xA5;
    retro_reset();
    saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(saveRam == stableSaveRamPointer && saveRam[0] == 0xA5,
           "system reset keeps the frontend-owned save RAM buffer stable");
    expect(retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM) == nullptr &&
               retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 0,
           "core does not report an unsupported memory type");
    retro_unload_game();
}

void testLibretroExecutesMbc1BankAndRestoresBatteryRam() {
    auto makeRom = [](bool incrementSavedValue) {
        std::vector<uint8_t> rom(0x20000, 0x00); // Eight 16 KiB banks.
        const uint8_t bankZeroProgram[] = {
            0x3E, 0x0A,             // LD A, $0A
            0xEA, 0x00, 0x00,       // LD ($0000), A (enable cartridge RAM)
            0x3E, 0x02,             // LD A, bank 2
            0xEA, 0x00, 0x20,       // LD ($2000), A (select ROM bank 2)
            0xCD, 0x00, 0x40,       // CALL $4000 (execute bank 2)
            0x18, 0xFE,             // Loop forever after returning.
        };
        std::copy(std::begin(bankZeroProgram), std::end(bankZeroProgram),
                  rom.begin() + 0x0100);

        if (incrementSavedValue) {
            const uint8_t bankTwoProgram[] = {
                0xFA, 0x00, 0xA0,   // LD A, ($A000)
                0x3C,               // INC A
                0xEA, 0x00, 0xA0,   // LD ($A000), A
                0xC9,               // RET
            };
            std::copy(std::begin(bankTwoProgram), std::end(bankTwoProgram),
                      rom.begin() + 0x8000);
        } else {
            const uint8_t bankTwoProgram[] = {
                0x3E, 0xA5,         // LD A, $A5
                0xEA, 0x00, 0xA0,   // LD ($A000), A
                0xC9,               // RET
            };
            std::copy(std::begin(bankTwoProgram), std::end(bankTwoProgram),
                      rom.begin() + 0x8000);
        }

        rom[0x0147] = 0x03; // MBC1 + RAM + battery
        rom[0x0148] = 0x02; // 128 KiB ROM
        rom[0x0149] = 0x02; // 8 KiB external RAM
        return rom;
    };

    std::vector<uint8_t> rom = makeRom(false);
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    expect(retro_load_game(&game),
           "core loads an MBC1 game with multiple ROM banks");
    for (unsigned frame = 0; frame < 2; ++frame)
        retro_run();

    expect(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0x2000,
           "MBC1 game exposes its declared battery RAM");
    auto *saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(saveRam != nullptr && saveRam[0] == 0xA5,
           "CPU executes bank 2 and writes its result to MBC1 battery RAM");
    const std::vector<uint8_t> restoredSave(saveRam, saveRam + 0x2000);

    retro_unload_game();
    rom = makeRom(true);
    game.data = rom.data();
    game.size = rom.size();
    expect(retro_load_game(&game),
           "core reloads a second MBC1 game instance");
    saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(saveRam != nullptr,
           "reloaded MBC1 game exposes a writable save block");
    std::copy(restoredSave.begin(), restoredSave.end(), saveRam);
    for (unsigned frame = 0; frame < 2; ++frame)
        retro_run();
    expect(saveRam[0] == 0xA6,
           "banked MBC1 code reads restored SRAM, updates it, and writes it back");
    retro_unload_game();
}

void testLibretroExposesMbc2BatteryRam() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x3E, 0x0A,             // LD A, $0A
        0xEA, 0x00, 0x00,       // LD ($0000), A (enable MBC2 RAM)
        0xFA, 0x00, 0xA0,       // LD A, ($A000) (upper nibble reads as $F)
        0x3C,                   // INC A
        0xEA, 0x00, 0xA0,       // LD ($A000), A (store the incremented nibble)
        0x18, 0xFE,             // loop forever
    };
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x0100);
    rom[0x0147] = 0x06; // MBC2 + battery
    rom[0x0148] = 0x00; // 32 KiB ROM
    rom[0x0149] = 0x00; // MBC2 RAM is built into the mapper

    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    expect(retro_load_game(&game),
           "core loads an original-header MBC2 battery cartridge");
    expect(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0x0200,
           "libretro reports MBC2 RAM as 512 bytes");
    auto *saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(saveRam != nullptr,
           "MBC2 battery cartridge exposes its save RAM buffer");
    saveRam[0] = 0x04; // Simulate a frontend restoring an existing .srm.
    retro_run();
    expect(saveRam[0] == 0x05,
           "MBC2 game reads restored progress and writes the updated nibble");

    const std::vector<uint8_t> frontendSave(saveRam, saveRam + 0x0200);
    uint8_t *stableSaveRamPointer = saveRam;
    retro_reset();
    saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(saveRam == stableSaveRamPointer && saveRam[0] == 0x05,
           "MBC2 system reset preserves the frontend-owned save buffer");

    retro_unload_game();
    expect(retro_load_game(&game),
           "core reloads the MBC2 battery cartridge after unloading it");
    expect(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0x0200,
           "reloaded MBC2 cartridge exposes the same 512-byte save RAM");
    saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(saveRam != nullptr, "reloaded MBC2 cartridge exposes save RAM data");
    std::copy(frontendSave.begin(), frontendSave.end(), saveRam);
    expect(saveRam[0] == 0x05,
           "frontend can restore MBC2 progress before execution");
    retro_run();
    expect(saveRam[0] == 0x06,
           "reloaded MBC2 game reads and advances its restored progress");
    retro_unload_game();
}

void testLibretroPersistsMbc5BatteryRamAcrossFrontendReload() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x3E, 0x0A,             // LD A, $0A
        0xEA, 0x00, 0x00,       // LD ($0000), A (enable cartridge RAM)
        0xFA, 0x00, 0xA0,       // LD A, ($A000) (read loaded progress)
        0x3C,                   // INC A
        0xEA, 0x00, 0xA0,       // LD ($A000), A (update saved progress)
        0x18, 0xFE,             // loop forever
    };
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x0100);
    rom[0x0147] = 0x1B; // MBC5 + RAM + battery
    rom[0x0148] = 0x00; // 32 KiB ROM
    rom[0x0149] = 0x02; // 8 KiB external RAM

    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    expect(retro_load_game(&game),
           "core loads an original-header MBC5 battery cartridge");
    expect(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0x2000,
           "libretro reports the MBC5 cartridge's declared RAM size");
    auto *saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(saveRam != nullptr, "MBC5 battery cartridge exposes save RAM data");
    saveRam[0] = 0x04; // Simulate a frontend restoring an existing .srm.
    retro_run();
    expect(saveRam[0] == 0x05,
           "MBC5 game reads restored progress and writes the updated value");

    const std::vector<uint8_t> frontendSave(saveRam, saveRam + 0x2000);
    retro_unload_game();
    expect(retro_load_game(&game),
           "core reloads the MBC5 battery cartridge after unloading it");
    expect(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0x2000,
           "reloaded MBC5 cartridge exposes the same save RAM size");
    saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(saveRam != nullptr, "reloaded MBC5 cartridge exposes save RAM data");
    std::copy(frontendSave.begin(), frontendSave.end(), saveRam);
    retro_run();
    expect(saveRam[0] == 0x06,
           "reloaded MBC5 game reads and advances its restored progress");
    retro_unload_game();
}

void testLibretroPersistsMbc3BatteryRamAcrossFrontendReload() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x3E, 0x0A,             // LD A, $0A
        0xEA, 0x00, 0x00,       // LD ($0000), A (enable cartridge RAM)
        0x3E, 0x02,             // LD A, $02
        0xEA, 0x00, 0x40,       // LD ($4000), A (select RAM bank 2)
        0xFA, 0x00, 0xA0,       // LD A, ($A000) (read loaded progress)
        0x3C,                   // INC A
        0xEA, 0x00, 0xA0,       // LD ($A000), A (update saved progress)
        0x18, 0xFE,             // loop forever
    };
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x0100);
    rom[0x0147] = 0x13; // MBC3 + RAM + battery
    rom[0x0148] = 0x00; // 32 KiB ROM
    rom[0x0149] = 0x03; // 32 KiB external RAM

    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    expect(retro_load_game(&game),
           "core loads an original-header MBC3 battery cartridge");

    const size_t saveRamSize = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    expect(saveRamSize == 0x8000,
           "libretro reports the MBC3 cartridge's declared external RAM size");
    auto *saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(saveRam != nullptr, "MBC3 battery cartridge exposes save RAM data");
    saveRam[0x4000] = 0x19;
    saveRam[0] = 0x44;
    retro_run();
    expect(saveRam[0x4000] == 0x1A,
           "MBC3 game reads restored progress and writes the updated value");
    expect(saveRam[0] == 0x44,
           "MBC3 game updates only the selected external RAM bank");

    const std::vector<uint8_t> frontendSave(saveRam, saveRam + saveRamSize);
    uint8_t *stableSaveRamPointer = saveRam;
    retro_reset();
    saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(saveRam == stableSaveRamPointer && saveRam[0x4000] == 0x1A,
           "MBC3 system reset preserves the frontend-owned save RAM buffer");

    retro_unload_game();
    expect(retro_load_game(&game),
           "core reloads the MBC3 battery cartridge after unloading it");
    expect(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == saveRamSize,
           "reloaded MBC3 cartridge exposes the same save RAM size");
    saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(saveRam != nullptr, "reloaded MBC3 cartridge exposes save RAM data");
    std::copy(frontendSave.begin(), frontendSave.end(), saveRam);
    expect(saveRam[0x4000] == 0x1A,
           "frontend can restore a saved MBC3 RAM bank before execution");
    retro_run();
    expect(saveRam[0x4000] == 0x1B,
           "reloaded MBC3 game reads and advances its restored progress");
    retro_unload_game();
}

void testLibretroExposesAndPreservesMbc3RtcMemory() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x0147] = 0x0F; // MBC3 + timer + battery
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();

    expect(retro_load_game(&game), "core loads an MBC3 timer cartridge");
    expect(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0 &&
               retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) == nullptr,
           "timer-only MBC3 cartridge has no save RAM block");
    expect(retro_get_memory_size(RETRO_MEMORY_RTC) == 24,
           "core exposes its versioned MBC3 RTC persistence block");
    auto *rtc = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_RTC));
    expect(rtc != nullptr, "MBC3 timer cartridge exposes RTC data");
    std::vector<uint8_t> savedRtc(rtc, rtc + 24);
    uint8_t *stableRtcPointer = rtc;

    retro_reset();
    rtc = static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_RTC));
    expect(rtc == stableRtcPointer &&
               std::equal(savedRtc.begin(), savedRtc.end(), rtc),
           "system reset retains the frontend-owned RTC memory block");

    retro_unload_game();
    expect(retro_load_game(&game),
           "core reloads the MBC3 timer cartridge after unloading it");
    rtc = static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_RTC));
    expect(rtc != nullptr, "reloaded MBC3 timer cartridge exposes RTC data");
    std::copy(savedRtc.begin(), savedRtc.end(), rtc);
    expect(std::equal(savedRtc.begin(), savedRtc.end(), rtc),
           "frontend can restore a saved MBC3 RTC block after load");
    retro_unload_game();

    rom[0x0147] = 0x10; // MBC3 + timer + RAM + battery
    rom[0x0149] = 0x02; // 8 KiB RAM
    expect(retro_load_game(&game),
           "core loads an MBC3 timer and RAM cartridge");
    expect(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0x2000 &&
               retro_get_memory_size(RETRO_MEMORY_RTC) == 24,
           "MBC3 timer and RAM cartridge exposes separate save blocks");
    retro_unload_game();
}

void testLibretroReportsLoadAndStoppedFrameDiagnostics() {
    logMessages.clear();
    logLevels.clear();
    expect(!retro_load_game(nullptr), "missing content fails with a diagnostic");
    expect(logMessages.size() == 1 && logLevels.back() == RETRO_LOG_ERROR,
           "frontend receives content error logging");

    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x147] = 0x22;
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    expect(!retro_load_game(&game), "unsupported mapper load fails");
    expect(logMessages.back().find("type=0x22 bytes=32768") != std::string::npos,
           "load diagnostic identifies mapper and ROM size");
    rom[0x147] = 0x03;
    rom[0x149] = 0xFF;
    expect(!retro_load_game(&game), "invalid mapper RAM header fails");
    expect(logMessages.back().find("unsupported MBC1 RAM size") != std::string::npos,
           "cartridge initialization error reaches frontend logging");

    rom[0x147] = 0;
    rom[0x149] = 0;
    rom[0x100] = 0x10; // STOP; ignore its zero padding byte.
    expect(retro_load_game(&game), "STOP diagnostic ROM loads");
    logMessages.clear();
    logLevels.clear();
    retro_run();
    retro_run();
    expect(logMessages.size() == 1 && logLevels.back() == RETRO_LOG_WARN &&
               logMessages.back().find("STOP") != std::string::npos,
           "stopped clock is reported once without flooding each frame");
    retro_reset();
    retro_run();
    expect(logMessages.size() == 2,
           "reset re-arms stopped-frame reporting for the new execution");
    retro_unload_game();

    rom[0x100] = 0x18; // JR -2: ordinary active execution.
    rom[0x101] = 0xFE;
    expect(retro_load_game(&game), "ordinary diagnostic ROM loads");
    logMessages.clear();
    retro_run();
    expect(logMessages.empty(), "normal complete frames produce no warnings");
    retro_unload_game();
}

void testLibretroReportsUnsupportedOpcodeAndCanReset() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x0147] = 0x00;
    rom[0x0100] = 0xD3;
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();

    expect(retro_load_game(&game), "unsupported-opcode diagnostic ROM loads");
    logMessages.clear();
    logLevels.clear();
    retro_run();
    expect(logMessages.size() == 1 && logLevels.back() == RETRO_LOG_ERROR &&
               logMessages.back().find("unsupported opcode 0xD3 at 0x0100") !=
                   std::string::npos,
           "frontend receives a precise unsupported-opcode error");

    retro_run();
    expect(logMessages.size() == 1,
           "a faulted unsupported-opcode run does not flood the frontend log");

    retro_reset();
    logMessages.clear();
    retro_run();
    expect(logMessages.size() == 1 && logLevels.back() == RETRO_LOG_ERROR,
           "reset clears the execution fault and reports it on the next run");
    retro_unload_game();
}

void testLibretroCallbackExceptionBoundaryFaultsOnce() {
    bool faulted = false;
    bool callbackRan = false;
    std::string diagnostic;
    const bool completed = LOTUSGB::LibretroInternal::invokeCallbackSafely(
        faulted,
        [&] {
            callbackRan = true;
            throw std::runtime_error("synthetic frame failure");
        },
        [&](const char *message) { diagnostic = message; });

    expect(callbackRan && !completed && faulted,
           "exception boundary catches a runtime callback failure and faults execution");
    expect(diagnostic == "synthetic frame failure",
           "exception boundary passes the exception message to the logger");

    bool retried = false;
    const bool retriedSuccessfully = LOTUSGB::LibretroInternal::invokeCallbackSafely(
        faulted, [&] { retried = true; }, [](const char *) {});
    expect(!retriedSuccessfully && !retried,
           "faulted execution does not re-enter the failing callback");

    bool unknownFaulted = false;
    std::string unknownDiagnostic;
    const bool unknownCompleted = LOTUSGB::LibretroInternal::invokeCallbackSafely(
        unknownFaulted, [] { throw 17; },
        [&](const char *message) { unknownDiagnostic = message; });
    expect(!unknownCompleted && unknownFaulted &&
               unknownDiagnostic == "unknown C++ exception",
           "exception boundary contains exceptions without a std::exception message");
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
    expect(avInfo.timing.sample_rate == 48000.0,
           "core reports the DMG audio output sample rate");
}

void testLibretroLoadsOptionalBootRomFromSystemDirectory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() /
                           ("lotusgb-bootrom-" + std::to_string(nonce));
    std::filesystem::create_directories(directory);
    systemDirectory = directory.string();

    const auto bootRomPath = directory / "dmg_boot.bin";
    {
        std::ofstream file(bootRomPath, std::ios::binary);
        std::array<uint8_t, 0x100> bootRom{};
        file.write(reinterpret_cast<const char *>(bootRom.data()),
                   static_cast<std::streamsize>(bootRom.size()));
        expect(static_cast<bool>(file), "synthetic boot ROM fixture is written");
    }

    const std::vector<uint8_t> rom = readBinary(
        std::string(LOTUSGB_LIBRETRO_TEST_ASSET_DIR) + "/gb-pong/gb-pong.gb");
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    logMessages.clear();
    expect(retro_load_game(&game),
           "core loads content when a valid optional DMG boot ROM is present");
    expect(!logMessages.empty() &&
               logMessages.back().find("Using DMG boot ROM") != std::string::npos,
           "core reports the boot ROM selected from the system directory");
    retro_unload_game();

    {
        std::ofstream file(bootRomPath, std::ios::binary | std::ios::trunc);
        std::array<uint8_t, 0xFF> invalidBootRom{};
        file.write(reinterpret_cast<const char *>(invalidBootRom.data()),
                   static_cast<std::streamsize>(invalidBootRom.size()));
        expect(static_cast<bool>(file), "invalid boot ROM fixture is written");
    }

    logMessages.clear();
    expect(!retro_load_game(&game),
           "core rejects an invalid-size DMG boot ROM instead of hiding the error");
    expect(!logMessages.empty() && logLevels.back() == RETRO_LOG_ERROR &&
               logMessages.back().find("exactly 256 bytes") != std::string::npos,
           "core explains why an invalid boot ROM cannot load");
    retro_unload_game();

    systemDirectory.clear();
    std::filesystem::remove_all(directory);
}

std::vector<uint8_t> makeFrameCoherenceRom(uint16_t startupDelay) {
    // Original synthetic ROM: switch every BG color between black and white
    // only in VBlank. Every complete frame must therefore be uniform. Delay
    // LCD enable so a frontend callback can land inside a visible PPU frame.
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3; // JP $0150; leave the cartridge header intact.
    rom[0x101] = 0x50;
    rom[0x102] = 0x01;
    const std::vector<uint8_t> program = {
        0xF3,                         // DI
        0xF0, 0x44,                   // wait: LDH A, (LY)
        0xFE, 0x90,                   // CP 144
        0x38, 0xFA,                   // JR C, wait
        0xAF,                         // XOR A
        0xE0, 0x40,                   // LCDC = 0 (disable in VBlank)
        0xE0, 0x47,                   // BGP = 0 (all white)
        0x01, static_cast<uint8_t>(startupDelay),
              static_cast<uint8_t>(startupDelay >> 8), // LD BC, delay
        0x0B,                         // delay: DEC BC
        0x78,                         // LD A, B
        0xB1,                         // OR C
        0x20, 0xFB,                   // JR NZ, delay
        0x06, 0x00,                   // LD B, 0 (current palette)
        0x3E, 0x91,                   // LD A, $91 (LCD + BG; no objects)
        0xE0, 0x40,                   // LDH (LCDC), A
        0xF0, 0x44,                   // visible: LDH A, (LY)
        0xFE, 0x90,                   // CP 144
        0x30, 0xFA,                   // JR NC, visible
        0xF0, 0x44,                   // vblank: LDH A, (LY)
        0xFE, 0x90,                   // CP 144
        0x38, 0xFA,                   // JR C, vblank
        0x78,                         // LD A, B
        0xEE, 0xFF,                   // XOR $FF
        0x47,                         // LD B, A
        0xE0, 0x47,                   // LDH (BGP), A
        0x18, 0xEC,                   // JR visible
    };
    std::copy(program.begin(), program.end(), rom.begin() + 0x150);
    return rom;
}

void testLibretroPublishesCoherentFramesAcrossLcdStartupPhases() {
    constexpr std::array<uint16_t, 5> STARTUP_DELAYS = {1, 127, 512, 1024, 2047};
    constexpr unsigned FRAMES_PER_DELAY = 40;
    for (const bool xrgb8888 : {true, false}) {
        retro_unload_game();
        retro_deinit();
        frontendAcceptsXrgb8888 = xrgb8888;
        retro_init();

        for (const uint16_t delay : STARTUP_DELAYS) {
            const auto rom = makeFrameCoherenceRom(delay);
            retro_game_info game{};
            game.data = rom.data();
            game.size = rom.size();
            logMessages.clear();
            logLevels.clear();
            expect(retro_load_game(&game), "frame-coherence probe loads");

            unsigned blackFrames = 0;
            unsigned whiteFrames = 0;
            for (unsigned frame = 0; frame < FRAMES_PER_DELAY; ++frame) {
                const unsigned previousVideoCount = videoRefreshCount;
                retro_run();
                expect(videoRefreshCount == previousVideoCount + 1,
                       "frame-coherence probe receives one callback per run");
                const uint32_t color = observedFrame.front();
                const auto mismatch = std::find_if(
                    observedFrame.begin(), observedFrame.end(),
                    [color](uint32_t pixel) { return pixel != color; });
                if (mismatch != observedFrame.end()) {
                    const size_t pixel = static_cast<size_t>(
                        mismatch - observedFrame.begin());
                    std::cerr << "Frame coherence mismatch: delay=" << delay
                              << " format=" << (xrgb8888 ? "XRGB8888" : "0RGB1555")
                              << " callback=" << frame << " pixel=("
                              << pixel % WIDTH << ',' << pixel / WIDTH << ")\n";
                }
                expect(mismatch == observedFrame.end(),
                       "video callback never mixes rows from alternating frames");
                expect(color == 0 || color == 0xFFFFFF,
                       "probe publishes only its programmed black/white palettes");
                blackFrames += color == 0;
                whiteFrames += color == 0xFFFFFF;
            }
            expect(blackFrames >= 10 && whiteFrames >= 10,
                   "uniform output must keep alternating, not freeze on one frame");
            expect(std::find(logLevels.begin(), logLevels.end(), RETRO_LOG_ERROR) ==
                       logLevels.end(),
                   "frame-coherence probe executes without core errors");
            retro_unload_game();
        }
    }
    retro_deinit();
    frontendAcceptsXrgb8888 = true;
    retro_init();
    std::cout << "Frame coherence: 400 callbacks across 5 LCD startup delays "
                 "and 2 pixel formats passed\n";
}

}

namespace {
void testLibretroSaveStateRoundTrip() {
    expect(retro_serialize_size() == 0, "no snapshot without content");
    expect(!retro_serialize(nullptr, 0) && !retro_unserialize(nullptr, 0), "null snapshot buffers rejected");
    const auto rom = readBinary(std::string(LOTUSGB_LIBRETRO_TEST_ASSET_DIR) + "/gb-pong/gb-pong.gb");
    retro_game_info game{}; game.data = rom.data(); game.size = rom.size();
    for (bool xrgb : {true, false}) {
        frontendAcceptsXrgb8888 = xrgb;
        retro_deinit(); retro_init();
        expect(retro_load_game(&game), "Pong loads for snapshot regression");
        upPressed = false;
        const auto size = retro_serialize_size();
        std::vector<uint8_t> initial(size);
        expect(size > 0 && retro_serialize(initial.data(), initial.size()), "save before first frame");
        for (unsigned i = 0; i < 90; ++i) retro_run();
        std::vector<uint8_t> saved(size);
        expect(retro_serialize(saved.data(), saved.size()), "save Pong mid-game");
        std::vector<std::array<uint32_t, WIDTH * HEIGHT>> frames;
        std::vector<std::vector<int16_t>> pcm;
        upPressed = true;
        for (unsigned i = 0; i < 30; ++i) {
            retro_run(); frames.push_back(observedFrame); pcm.push_back(latestAudio);
        }
        std::vector<uint8_t> expected(size);
        expect(retro_serialize(expected.data(), expected.size()), "save expected future");
        expect(expected != saved, "game state advances after checkpoint");
        expect(retro_serialize_size() == size, "libretro serialization size remains fixed");
        // Fresh content load reconstructs all pointers and decoder instances.
        retro_unload_game();
        expect(retro_load_game(&game), "reload same ROM before restoring snapshot");
        expect(retro_unserialize(saved.data(), saved.size()), "restore Pong in fresh system");
        for (unsigned i = 0; i < 30; ++i) {
            retro_run();
            expect(observedFrame == frames[i], "Pong restored video matches every subsequent frame");
            expect(latestAudio == pcm[i], "Pong restored PCM matches every subsequent batch");
        }
        std::vector<uint8_t> actual(size);
        expect(retro_serialize(actual.data(), actual.size()) && actual == expected,
               "Pong restored complete state matches expected future");
        auto bad = saved; bad.back() = 1;
        expect(!retro_unserialize(bad.data(), bad.size()), "nonzero snapshot padding rejected");
        expect(!retro_unserialize(saved.data(), 100), "truncated libretro snapshot rejected");
        expect(!retro_serialize(actual.data(), size - 1), "undersized output rejected");
        expect(retro_serialize(actual.data(), actual.size()) && actual == expected,
               "failed snapshot operations preserve running game");
        const auto logsBefore = logMessages.size();
        retro_run();
        expect(logMessages.size() == logsBefore, "rejected state does not fault next frame");
        expect(retro_unserialize(initial.data(), initial.size()), "restore pre-first-frame snapshot");
        expect(retro_serialize(actual.data(), actual.size()) && actual == initial, "initial state restored exactly");
        retro_unload_game();
    }
    frontendAcceptsXrgb8888 = true; upPressed = false;
    retro_deinit(); retro_init();
}
}

int main() {
    retro_set_environment(environmentCallback);
    retro_set_video_refresh(videoRefreshCallback);
    retro_set_audio_sample_batch(audioSampleBatchCallback);
    retro_set_input_poll(inputPollCallback);
    retro_set_input_state(inputStateCallback);
    retro_init();

    testLibretroReportsApiAndVideoTiming();
    testLibretroLoadsOptionalBootRomFromSystemDirectory();
    testLibretroLoadsAcid2AndPublishesReferenceFrame();
    testLibretroLoadsMaximumMbc5RomSize();
    testLibretroInputChangesPongGameplayFrame();
    testLibretroPublishesPulseChannelAudio();
    testLibretroExposesMbc1BatteryRam();
    testLibretroExecutesMbc1BankAndRestoresBatteryRam();
    testLibretroExposesMbc2BatteryRam();
    testLibretroPersistsMbc5BatteryRamAcrossFrontendReload();
    testLibretroPersistsMbc3BatteryRamAcrossFrontendReload();
    testLibretroExposesAndPreservesMbc3RtcMemory();
    testLibretroReportsLoadAndStoppedFrameDiagnostics();
    testLibretroReportsUnsupportedOpcodeAndCanReset();
    testLibretroCallbackExceptionBoundaryFaultsOnce();
    testLibretroPublishesCoherentFramesAcrossLcdStartupPhases();
    testLibretroSaveStateRoundTrip();

    retro_unload_game();
    retro_deinit();
    std::cout << "Libretro adapter tests passed\n";
}
