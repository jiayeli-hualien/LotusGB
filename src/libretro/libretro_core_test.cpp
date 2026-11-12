#include "libretro.h"
#include "callback_exception_boundary.hpp"
#include "../../tests/include/lcd_cadence_rom.hpp"

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
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>
#include <cerrno>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include "../../tests/include/cgb_test_rom.hpp"
#include "../../tests/include/cgb_compat_rom.hpp"

#ifndef LOTUSGB_LIBRETRO_TEST_ASSET_DIR
#error LOTUSGB_LIBRETRO_TEST_ASSET_DIR must name the test ROM assets directory
#endif

namespace {

constexpr unsigned WIDTH = 160;
constexpr unsigned HEIGHT = 144;

unsigned inputPollCount = 0;
unsigned videoRefreshCount = 0;
unsigned observedPitch = 0;
std::string hardwareModelOption = "auto";
bool modelOptionAdvertised = false;
bool throwDuringModelRegistration = false;
bool frontendAcceptsXrgb8888 = true;
bool upPressed = false;
bool latchUpOnPoll = false;
bool pendingUpPressed = false;
unsigned buttonsPressed = 0;
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
bool accelerometerAvailable = false;
bool accelerometerEnabled = false;
unsigned accelerometerEnableCalls = 0;
unsigned accelerometerDisableCalls = 0;
unsigned accelerometerReadCalls = 0;
float accelerometerX = 0.5f;
float accelerometerY = -0.25f;
bool rumbleInterfaceAvailable = true;
std::vector<uint16_t> rumbleRequests;

bool frontendSetSensorState(unsigned port, retro_sensor_action action,
                            unsigned rate) {
    expect(port == 0, "cartridge accelerometer uses the primary controller port");
    if (action == RETRO_SENSOR_ACCELEROMETER_ENABLE) {
        expect(rate == 60, "core requests a frame-rate accelerometer sample");
        if (!accelerometerAvailable)
            return false;
        accelerometerEnabled = true;
        ++accelerometerEnableCalls;
        return true;
    }
    if (action == RETRO_SENSOR_ACCELEROMETER_DISABLE) {
        accelerometerEnabled = false;
        ++accelerometerDisableCalls;
        return true;
    }
    return false;
}

float frontendGetSensorInput(unsigned port, unsigned id) {
    expect(port == 0 && accelerometerEnabled,
           "core samples only an enabled primary-port accelerometer");
    ++accelerometerReadCalls;
    if (id == RETRO_SENSOR_ACCELEROMETER_X)
        return accelerometerX;
    if (id == RETRO_SENSOR_ACCELEROMETER_Y)
        return accelerometerY;
    return 0;
}

bool frontendSetRumbleState(unsigned port, retro_rumble_effect effect,
                            uint16_t strength) {
    expect(port == 0 && effect == RETRO_RUMBLE_STRONG,
           "cartridge rumble uses the primary strong motor");
    rumbleRequests.push_back(strength);
    return true;
}

void frontendLog(retro_log_level level, const char *format, ...) {
    char buffer[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    logMessages.emplace_back(buffer);
    logLevels.push_back(level);
}

const retro_subsystem_info *linkSubsystem = nullptr;
const retro_netpacket_callback *netpacketInterface = nullptr;
unsigned mockSerialRequests = 0;
unsigned mockDisconnectPolls = 0;
int processNetpacketSocket = -1;
uint16_t processNetpacketPeerId = 0;

int processNetpacketSendFlags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

void mockNetpacketSend(int flags, const void *data, size_t size, uint16_t clientId) {
    expect((flags & RETRO_NETPACKET_RELIABLE) != 0 &&
           (flags & RETRO_NETPACKET_FLUSH_HINT) != 0 &&
           clientId == RETRO_NETPACKET_BROADCAST,
           "serial link sends reliable broadcast packets immediately");
    const auto *request = static_cast<const uint8_t *>(data);
    expect(size == 10 && request[4] == 1, "core sends a serial edge request");
    ++mockSerialRequests;
    std::array<uint8_t, 10> response{};
    std::copy(request, request + size, response.begin());
    response[4] = 2;
    response[5] = 0;
    netpacketInterface->receive(response.data(), response.size(), 0);
}

void mockNetpacketPollReceive() {}

void mockNetpacketSendWithoutReply(int flags, const void *data, size_t size,
                                   uint16_t clientId) {
    expect((flags & RETRO_NETPACKET_RELIABLE) != 0 &&
               (flags & RETRO_NETPACKET_FLUSH_HINT) != 0 &&
               clientId == RETRO_NETPACKET_BROADCAST && data && size == 10 &&
               static_cast<const uint8_t *>(data)[4] == 1,
           "host starts the expected reliable serial request before disconnect");
    ++mockSerialRequests;
}

void mockNetpacketDisconnectDuringPoll() {
    ++mockDisconnectPolls;
    netpacketInterface->disconnected(1);
}

void processNetpacketSend(int flags, const void *data, size_t size,
                          uint16_t clientId) {
    expect((flags & RETRO_NETPACKET_RELIABLE) != 0 &&
               (flags & RETRO_NETPACKET_FLUSH_HINT) != 0 &&
               clientId == RETRO_NETPACKET_BROADCAST && data && size == 10 &&
               processNetpacketSocket >= 0,
           "two-process serial traffic uses reliable broadcast packets");
    const ssize_t written = ::send(processNetpacketSocket, data, size,
                                   processNetpacketSendFlags());
    expect(written == static_cast<ssize_t>(size),
           "two-process serial packet reaches the relay");
}

void processNetpacketPollReceive() {
    std::array<uint8_t, 64 * 1024> packet{};
    while (true) {
        const ssize_t size = ::recv(processNetpacketSocket, packet.data(),
                                    packet.size(), MSG_DONTWAIT);
        if (size < 0 && errno == EINTR)
            continue;
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        expect(size > 0, "two-process serial receive remains connected");
        netpacketInterface->receive(packet.data(), static_cast<size_t>(size),
                                    processNetpacketPeerId);
    }
}

bool environmentCallback(unsigned command, void *data) {
    if (command == RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO) {
        linkSubsystem = static_cast<const retro_subsystem_info *>(data);
        return true;
    }
    if (command == RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE) {
        netpacketInterface = static_cast<const retro_netpacket_callback *>(data);
        return true;
    }
    if (command == RETRO_ENVIRONMENT_SET_VARIABLES) {
        if (throwDuringModelRegistration) throw std::runtime_error("model option registration failure");
        const auto *variables = static_cast<const retro_variable *>(data);
        for (; variables->key; ++variables) {
            if (std::string(variables->key) == "lotusgb_model") {
                modelOptionAdvertised = std::string(variables->value).find("auto|dmg|cgb") != std::string::npos;
            }
        }
        return true;
    }
    if (command == RETRO_ENVIRONMENT_GET_VARIABLE) {
        auto *variable = static_cast<retro_variable *>(data);
        if (std::string(variable->key) != "lotusgb_model" || hardwareModelOption.empty()) return false;
        variable->value = hardwareModelOption.c_str(); return true;
    }
    if (command == RETRO_ENVIRONMENT_GET_LOG_INTERFACE) {
        static_cast<retro_log_callback *>(data)->log = frontendLog;
        return true;
    }
    if (command == RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE) {
        auto *interface = static_cast<retro_sensor_interface *>(data);
        interface->set_sensor_state = frontendSetSensorState;
        interface->get_sensor_input = frontendGetSensorInput;
        return true;
    }
    if (command == RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE) {
        if (!rumbleInterfaceAvailable)
            return false;
        static_cast<retro_rumble_interface *>(data)->set_rumble_state =
            frontendSetRumbleState;
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
    if (latchUpOnPoll)
        upPressed = pendingUpPressed;
}

int16_t inputStateCallback(unsigned port, unsigned device, unsigned index,
                           unsigned id) {
    if (port == 0 && device == RETRO_DEVICE_JOYPAD && index == 0 && id < 16 &&
        (buttonsPressed & (1u << id)))
        return 1;
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
    unsupported[0x0147] = 0x20; // MBC6 remains unsupported.
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

void testLibretroPongInputResponse() {
    // Compare identical saved game states, restricting observations to the
    // player's paddle so ball/score animation cannot count as input response.
    const auto rom = readBinary(std::string(LOTUSGB_LIBRETRO_TEST_ASSET_DIR) +
                                "/gb-pong/gb-pong.gb");
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    constexpr unsigned WINDOW = 6;
    constexpr unsigned SAMPLES = 24;
    using Paddle = std::array<uint32_t, 8 * HEIGHT>;
    const auto paddle = [] {
        Paddle result{};
        for (unsigned y = 0; y < HEIGHT; ++y)
            std::copy_n(observedFrame.begin() + y * WIDTH + 4, 8,
                        result.begin() + y * 8);
        return result;
    };
    for (bool xrgb : {true, false}) {
        retro_unload_game();
        retro_deinit();
        frontendAcceptsXrgb8888 = xrgb;
        retro_init();
        expect(retro_load_game(&game), "Pong loads for input response measurement");
        logLevels.clear();
        latchUpOnPoll = true;
        pendingUpPressed = false;
        for (unsigned i = 0; i < 60; ++i) retro_run();
        std::array<unsigned, WINDOW + 1> pressHistogram{};
        std::array<unsigned, WINDOW + 1> releaseHistogram{};
        for (unsigned sample = 0; sample < SAMPLES; ++sample) {
            std::vector<uint8_t> saved(retro_serialize_size());
            expect(retro_serialize(saved.data(), saved.size()), "save neutral Pong checkpoint");
            std::array<std::array<Paddle, WINDOW>, 3> branches{};
            for (unsigned branch = 0; branch < branches.size(); ++branch) {
                expect(retro_unserialize(saved.data(), saved.size()), "restore input comparison checkpoint");
                for (unsigned frame = 0; frame < WINDOW; ++frame) {
                    // Neutral, held Up, and a one-poll Up pulse followed by release.
                    pendingUpPressed = branch == 1 || (branch == 2 && frame == 0);
                    const unsigned polls = inputPollCount;
                    const unsigned videos = videoRefreshCount;
                    retro_run();
                    expect(inputPollCount == polls + 1 && videoRefreshCount == videos + 1,
                           "input measurement receives one fresh poll and video per run");
                    branches[branch][frame] = paddle();
                }
            }
            unsigned press = 0, pulse = 0, release = 0;
            for (unsigned frame = 0; frame < WINDOW; ++frame) {
                if (!press && branches[1][frame] != branches[0][frame]) press = frame + 1;
                if (!pulse && branches[2][frame] != branches[0][frame]) pulse = frame + 1;
                if (frame > 0 && !release && branches[2][frame] != branches[1][frame])
                    release = frame; // Calls counted from the release poll.
            }
            expect(press > 0 && press <= 3, "Pong paddle responds within three calls including press");
            expect(pulse == press, "one-poll Up pulse is visible at the same time as held Up");
            expect(release > 0 && release <= 3, "Pong responds within three calls including release");
            ++pressHistogram[press];
            ++releaseHistogram[release];
            expect(retro_unserialize(saved.data(), saved.size()), "return to neutral sample timeline");
            pendingUpPressed = false;
            retro_run();
        }
        std::cout << "Pong input response (" << (xrgb ? "XRGB8888" : "0RGB1555")
                  << "): " << SAMPLES << " checkpoints, press/release call histograms:";
        for (unsigned call = 1; call <= WINDOW; ++call)
            std::cout << ' ' << call << '=' << pressHistogram[call] << '/'
                      << releaseHistogram[call];
        std::cout << "; one-poll pulses preserved\n";
        expect(std::find(logLevels.begin(), logLevels.end(), RETRO_LOG_ERROR) == logLevels.end(),
               "Pong input measurement executes without core errors");
        latchUpOnPoll = false;
        pendingUpPressed = upPressed = false;
        retro_unload_game();
    }
    retro_deinit();
    frontendAcceptsXrgb8888 = true;
    retro_init();
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
#ifndef LOTUSGB_TEST_FIXED_FRAME_STEP
    constexpr size_t FIRST_AUDIO_BATCH = 751, FIRST_TWO_TOTAL = 1555;
#else
    constexpr size_t FIRST_AUDIO_BATCH = 803, FIRST_TWO_TOTAL = 1607;
#endif
    expect(audioBatchSizes.size() == 2 && audioBatchSizes[0] == FIRST_AUDIO_BATCH &&
               audioBatchSizes[1] == 804 && audioFrameCount == FIRST_TWO_TOTAL,
           "audio sample batches follow the 48 kHz clock across video frames");
    expect(nonzeroAudioSampleCount > 0,
           "channel 1 ROM output reaches the frontend as non-silent PCM");
    constexpr uint64_t CALLS = 240, FRAME_T = 70224;
    for (unsigned call = 2; call < CALLS; ++call) {
        retro_run();
        expect(audioBatchSizes.back() == 803 || audioBatchSizes.back() == 804,
               "steady LCD-on callbacks keep the nominal 803/804 PCM cadence");
    }
#ifndef LOTUSGB_TEST_FIXED_FRAME_STEP
    constexpr uint64_t TOTAL_T = 65664 + (CALLS - 1) * FRAME_T;
#else
    constexpr uint64_t TOTAL_T = CALLS * FRAME_T;
#endif
    expect(audioBatchCallCount == CALLS && audioFrameCount == TOTAL_T * 48000 / 4194304,
           "longer audio run has no cumulative sample drift for its emulated time");
    std::cout << "Steady audio: " << CALLS << " callbacks, " << audioFrameCount
              << " stereo frames, exact 48 kHz clock total\n";
    retro_unload_game();
}

void testLibretroLcdRestartKeepsProgressAndCoherentCallbacks() {
    const auto rom = TestRom::lcdRestartCadence();
    retro_game_info game{}; game.data = rom.data(); game.size = rom.size();
    for (bool xrgb : {true, false}) {
        retro_deinit(); frontendAcceptsXrgb8888 = xrgb; retro_init();
        expect(retro_load_game(&game), "LCD restart cadence ROM loads");
        logLevels.clear(); audioFrameCount = 0;
        unsigned black = 0, white = 0;
        for (unsigned call = 0; call < 60; ++call) {
            const auto polls = inputPollCount, videos = videoRefreshCount;
            retro_run();
            expect(inputPollCount == polls + 1 && videoRefreshCount == videos + 1,
                   "LCD restart keeps one input poll and video callback per run");
            const auto color = observedFrame.front();
            expect((color == 0 || color == 0xFFFFFF) &&
                       std::all_of(observedFrame.begin(), observedFrame.end(),
                                   [color](uint32_t pixel) { return pixel == color; }),
                   "LCD restart never submits a mixed unfinished frame");
            black += color == 0; white += color == 0xFFFFFF;
        }
        const auto *ram = static_cast<const uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
        expect(ram && ram[0] > 10 && white > 0,
               "LCD restart keeps CPU progress and observes LCD-off output");
        expect(std::find(logLevels.begin(), logLevels.end(), RETRO_LOG_ERROR) == logLevels.end(),
               "LCD restart does not fault emulation");
#ifndef LOTUSGB_TEST_FIXED_FRAME_STEP
        expect(black > 0, "VBlank stepping observes the completed black frames before LCD-off clears them");
        expect(audioFrameCount < 60 * 70224ULL * 48000 / 4194304,
               "VBlank restart exposes shorter time per callback instead of hiding it with padded PCM");
        expect(logLevels.empty(),
               "bounded presentation repeats during LCD restart are expected and produce no fault diagnostics");
#else
        // A fixed callback need not intersect the short interval between
        // completion and LCD-off clearing the snapshot. Record the observed
        // colors instead of asserting it sees each intermediate LCD state.
        expect(audioFrameCount == 60 * 70224ULL * 48000 / 4194304,
               "fixed stepping retains nominal audio/time cadence through LCD restart");
#endif
        std::cout << "LCD restart (" << (xrgb ? "XRGB8888" : "0RGB1555")
                  << "): 60 callbacks, " << audioFrameCount << " audio frames, "
                  << static_cast<unsigned>(ram[0]) << " restart iterations, black/white="
                  << black << '/' << white << '\n';
        retro_unload_game();
    }
    retro_deinit(); frontendAcceptsXrgb8888 = true; retro_init();
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

void testMbc7LibretroSensorAndBatteryContract() {
    retro_unload_game();
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x3E, 0x0A,             // LD A, $0A
        0xEA, 0x00, 0x00,       // LD ($0000), A (MBC7 gate 1)
        0x3E, 0x48,             // LD A, $48 (register gate and rumble)
        0xEA, 0x00, 0x40,       // LD ($4000), A
        0x18, 0xFE,             // loop forever
    };
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x0100);
    rom[0x0143] = 0x80;
    rom[0x0147] = 0x22;
    rom[0x0148] = 0;
    rom[0x0149] = 0;
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();

    accelerometerAvailable = true;
    accelerometerEnabled = false;
    const unsigned readsBefore = accelerometerReadCalls;
    const unsigned enablesBefore = accelerometerEnableCalls;
    const unsigned disablesBefore = accelerometerDisableCalls;
    rumbleRequests.clear();
    expect(retro_load_game(&game), "libretro accepts MBC7 cartridges");
    expect(accelerometerEnabled &&
               accelerometerEnableCalls == enablesBefore + 1,
           "MBC7 content enables the frontend accelerometer");
    auto *saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(saveRam && retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0x100,
           "libretro exposes the MBC7 EEPROM as a 256-byte battery block");
    retro_run();
    expect(accelerometerReadCalls == readsBefore + 2,
           "each MBC7 run samples both accelerometer axes");
    expect(rumbleRequests == std::vector<uint16_t>{0xFFFF},
           "MBC7 register bit 3 drives the frontend rumble motor");
    std::vector<uint8_t> snapshot(retro_serialize_size());
    expect(retro_serialize(snapshot.data(), snapshot.size()) && snapshot[8] == 13,
           "MBC7 sensor and rumble probe serialize in the current CGB state format");
    const uint32_t usedSize = static_cast<uint32_t>(snapshot[16]) |
        (static_cast<uint32_t>(snapshot[17]) << 8) |
        (static_cast<uint32_t>(snapshot[18]) << 16) |
        (static_cast<uint32_t>(snapshot[19]) << 24);
    expect(usedSize >= 42,
           "MBC7 snapshot includes its live cartridge sensor values");
    const size_t mbc7State = usedSize - 42;
    const uint16_t savedX = static_cast<uint16_t>(snapshot[mbc7State + 3] |
        (static_cast<uint16_t>(snapshot[mbc7State + 4]) << 8));
    const uint16_t savedY = static_cast<uint16_t>(snapshot[mbc7State + 5] |
        (static_cast<uint16_t>(snapshot[mbc7State + 6]) << 8));
    expect(savedX == 0x8198 && savedY == 0x81B4,
           "libretro acceleration maps rightward X negative and upward Y positive into MBC7 units");
    expect(snapshot[mbc7State + 1] == 0x48,
           "CGB snapshot preserves enabled MBC7 RAM gate and active rumble bit");
    retro_reset();
    expect(accelerometerEnabled,
           "system reset keeps the MBC7 frontend sensor enabled");
    expect(rumbleRequests == std::vector<uint16_t>({0xFFFF, 0}),
           "reset stops MBC7 rumble before replacing the cartridge instance");
    expect(retro_unserialize(snapshot.data(), snapshot.size()) &&
               rumbleRequests == std::vector<uint16_t>({0xFFFF, 0, 0xFFFF}),
           "restoring an active MBC7 snapshot restarts frontend rumble");
    retro_run();
    expect(accelerometerReadCalls == readsBefore + 4,
           "MBC7 samples resume after a system reset");
    retro_unload_game();
    expect(!accelerometerEnabled &&
               accelerometerDisableCalls == disablesBefore + 1,
           "unloading MBC7 disables its frontend accelerometer");
    expect(rumbleRequests == std::vector<uint16_t>({0xFFFF, 0, 0xFFFF, 0}),
           "unloading an MBC7 game stops the frontend rumble motor");

    accelerometerAvailable = false;
    logMessages.clear();
    expect(retro_load_game(&game),
           "MBC7 remains usable when the frontend has no accelerometer");
    expect(!accelerometerEnabled &&
               std::any_of(logMessages.begin(), logMessages.end(),
                   [](const std::string &message) {
                       return message.find("neutral sensor values") !=
                              std::string::npos;
                   }),
           "missing sensor support is reported with a neutral-value fallback");
    retro_run();
    expect(accelerometerReadCalls == readsBefore + 4,
           "neutral MBC7 fallback does not call unavailable sensor input");
    retro_unload_game();
    accelerometerAvailable = false;
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

void testLibretroMbc5RumbleAndSnapshotRestore() {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x3E, 0x0A,             // LD A, $0A
        0xEA, 0x00, 0x00,       // LD ($0000), A (enable cartridge RAM)
        0x3E, 0x0B,             // LD A, $0B (rumble on, RAM bank 3)
        0xEA, 0x00, 0x40,       // LD ($4000), A
        0x18, 0xFE,             // loop forever
    };
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x0100);
    rom[0x0147] = 0x1E; // MBC5 + rumble + RAM + battery
    rom[0x0148] = 0x00;
    rom[0x0149] = 0x03; // 32 KiB, four 8-KiB RAM banks

    rumbleRequests.clear();
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    expect(retro_load_game(&game), "core loads an original-header MBC5 rumble cartridge");
    std::vector<uint8_t> initial(retro_serialize_size());
    expect(retro_serialize(initial.data(), initial.size()),
           "save MBC5 rumble state before guest register writes");

    retro_run();
    expect(rumbleRequests == std::vector<uint16_t>{0xFFFF},
           "MBC5 motor bit requests strong rumble through the libretro interface");
    expect(retro_unserialize(initial.data(), initial.size()),
           "restore the MBC5 snapshot with its motor switched off");
    expect(rumbleRequests == std::vector<uint16_t>({0xFFFF, 0}),
           "state restore synchronizes the frontend motor to the saved bank register");

    retro_run();
    expect(rumbleRequests == std::vector<uint16_t>({0xFFFF, 0, 0xFFFF}),
           "guest rumble writes resume after restoring the pre-write snapshot");
    retro_unload_game();
    expect(rumbleRequests == std::vector<uint16_t>({0xFFFF, 0, 0xFFFF, 0}),
           "unloading an MBC5 rumble game turns the frontend motor off");
}

void testLibretroMbc5RumbleWithoutFrontendSupport() {
    retro_deinit();
    rumbleInterfaceAvailable = false;
    logMessages.clear();
    retro_init();

    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x3E, 0x08,             // LD A, $08 (rumble on)
        0xEA, 0x00, 0x40,       // LD ($4000), A
        0x18, 0xFE,             // loop forever
    };
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x0100);
    rom[0x0147] = 0x1C; // MBC5 + rumble, no RAM
    rom[0x0148] = 0x00;

    const size_t priorRequests = rumbleRequests.size();
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    expect(retro_load_game(&game),
           "rumble cartridge remains playable without the optional frontend interface");
    retro_run();
    expect(rumbleRequests.size() == priorRequests &&
               std::any_of(logMessages.begin(), logMessages.end(),
                   [](const std::string &message) {
                       return message.find("rumble is unavailable") != std::string::npos;
                   }),
           "missing frontend rumble is reported and gameplay continues without motor output");
    retro_unload_game();
    rumbleInterfaceAvailable = true;
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
    rom[0x147] = 0x20;
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    expect(!retro_load_game(&game), "unsupported mapper load fails");
    expect(logMessages.back().find("type=0x20 bytes=32768") != std::string::npos,
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

void testLibretroIllegalOpcodeLocksWithoutFaultingFrontend() {
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x0147] = 0x00;
    rom[0x0100] = 0xD3;
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();

    expect(retro_load_game(&game), "illegal-opcode diagnostic ROM loads");
    logMessages.clear();
    logLevels.clear();
    retro_run();
    expect(logMessages.empty(),
           "hardware CPU lock-up does not become a frontend execution error");

    retro_run();
    expect(logMessages.empty(),
           "continued frontend callbacks remain free of lock-up error reports");

    retro_reset();
    logMessages.clear();
    retro_run();
    expect(logMessages.empty(),
           "frontend reset restarts execution without treating the illegal opcode as a core fault");
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
               std::string(info.valid_extensions) == "gb|gbc",
           "core advertises both Game Boy and Game Boy Color ROM content");
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

void testLibretroCgbCompatibilitySelection() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("lotusgb-compat-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto boot = LOTUSGB::Test::makeCgbCompatibilityBootRom();
    {
        std::ofstream file(directory / "cgb_boot.bin", std::ios::binary);
        file.write(reinterpret_cast<const char *>(boot.data()), boot.size());
        expect(bool(file), "write original compatibility bootstrap");
    }
    auto rom = LOTUSGB::Test::makeCgbCompatibilityRom();
    retro_game_info game{}; game.data = rom.data(); game.size = rom.size();
    for (bool xrgb : {true, false}) {
        retro_deinit(); frontendAcceptsXrgb8888 = xrgb; retro_init();
        logMessages.clear(); logLevels.clear();
        expect(modelOptionAdvertised, "frontend can choose automatic, DMG or CGB hardware");
        hardwareModelOption = "cgb"; systemDirectory = directory.string();
        expect(retro_load_game(&game), "forced CGB loads a monochrome cartridge and CGB bootstrap");
        for (unsigned frame = 0; frame < 4; ++frame) retro_run();
        const uint32_t green = 0x0000FF00; // Callback normalizes both pixel formats to RGB888.
        expect(std::all_of(observedFrame.begin(), observedFrame.end(), [&](uint32_t x) { return x == green; }),
               "compatibility RGB555 palette reaches both frontend pixel formats");
        auto *ram = static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
        expect(ram && ram[0] == 0x11 && ram[0x40] == 1 && ram[2] == 0xFF,
               "frontend compatibility cartridge executes and cannot access KEY1");
        std::vector<uint8_t> saved(retro_serialize_size()), future(saved.size()), replay(saved.size());
        expect(retro_serialize(saved.data(), saved.size()) && saved[8] == 13 && saved[12] == 2,
               "frontend exposes current CGB physical-model states for compatibility games");
        retro_run(); const auto video = observedFrame; const auto audio = latestAudio;
        expect(retro_serialize(future.data(), future.size()), "capture compatibility future");
        expect(retro_unserialize(saved.data(), saved.size()), "restore compatibility frontend state");
        retro_run(); expect(observedFrame == video && latestAudio == audio &&
            retro_serialize(replay.data(), replay.size()) && replay == future, "compatibility rewind restores exact video, audio and complete state");
        hardwareModelOption = "auto"; retro_reset(); // Changes apply on next content load.
        for (unsigned frame = 0; frame < 4; ++frame) retro_run();
        expect(retro_serialize(replay.data(), replay.size()) && replay[12] == 2 && observedFrame == video,
               "reset preserves selected physical hardware and repeats CGB compatibility bootstrap");
        retro_unload_game(); systemDirectory.clear();
        expect(retro_load_game(&game), "automatic model selection accepts monochrome content");
        expect(retro_serialize(replay.data(), replay.size()) && replay[12] == 1,
               "automatic selection still chooses actual DMG hardware for monochrome headers");
        retro_unload_game(); hardwareModelOption.clear();
        expect(retro_load_game(&game) && retro_serialize(replay.data(), replay.size()) && replay[12] == 1,
               "frontends without variables retain header-based model selection");
        retro_unload_game(); hardwareModelOption = "cgb";
        expect(retro_load_game(&game) && retro_serialize(replay.data(), replay.size()) && replay[12] == 2,
               "forced compatibility also supports no-BIOS synthetic startup");
        retro_unload_game(); hardwareModelOption = "dmg";
        rom[0x143] = 0x80;
        expect(retro_load_game(&game) && retro_serialize(replay.data(), replay.size()) && replay[12] == 1,
               "explicit DMG selection overrides a dual-mode cartridge header");
        retro_unload_game(); rom[0x143] = 0;
        expect(std::find(logLevels.begin(), logLevels.end(), RETRO_LOG_ERROR) == logLevels.end(), "compatibility frontend checks report no unexpected core errors");
        hardwareModelOption = "invalid";
        expect(!retro_load_game(&game), "invalid model values fail explicitly");
        hardwareModelOption = "auto"; logMessages.clear(); logLevels.clear();
    }
    std::filesystem::remove_all(directory);
    retro_deinit(); throwDuringModelRegistration = true;
    retro_init();
    expect(!retro_load_game(&game), "environment exceptions stay inside the C ABI and fault initialization");
    throwDuringModelRegistration = false;
    retro_deinit(); frontendAcceptsXrgb8888 = true; retro_init();
    expect(retro_load_game(&game), "reinitialization recovers from an environment callback failure");
    retro_unload_game(); logMessages.clear(); logLevels.clear();
}

void testLibretroCgbBootRenderingStateAndReset() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("lotusgb-cgb-boot-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    systemDirectory = directory.string();
    std::vector<uint8_t> boot(0x900, 0);
    boot[0] = 0xC3; boot[1] = 0; boot[2] = 2; // Execute the second boot window.
    boot[0xFC] = 0x3E; boot[0xFD] = 0x11; boot[0xFE] = 0xE0; boot[0xFF] = 0x50;
    size_t at = 0x200;
    const auto emit = [&](std::initializer_list<uint8_t> bytes) { for (auto b : bytes) boot[at++] = b; };
    const auto io = [&](uint8_t port, uint8_t value) { emit({0x3E, value, 0xE0, port}); };
    emit({0xF3, 0x31, 0xFE, 0xFF, 0x3E, 0x0A, 0xEA, 0, 0});
    emit({0xFA, 0x43, 1, 0xEA, 0x20, 0xA0}); // Header hole remains cartridge data.
    io(0x68, 0x80); io(0x69, 0); io(0x69, 0x7C); io(0x40, 0x91);
    emit({0x06, 4});
    const size_t loop = at;
    emit({0xF0, 0x44, 0xFE, 0x90, 0x20, 0xFA});
    emit({0xF0, 0x44, 0xFE, 0, 0x20, 0xFA, 0x05});
    emit({0x20, static_cast<uint8_t>(loop - at - 2)});
    emit({0xC3, 0xFC, 0}); // Unmap from the first window at $FE; game starts at $100.
    const auto writeBoot = [&](const char *name, const std::vector<uint8_t> &bytes) {
        std::ofstream file(directory / name, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        expect(bool(file), "write original CGB boot fixture");
    };
    writeBoot("cgb_boot.bin", boot);
    auto rom = LOTUSGB::Test::makeCgbMbc1BatteryExerciseRom();
    retro_game_info game{}; game.data = rom.data(); game.size = rom.size();
    buttonsPressed = 0;
    expect(retro_load_game(&game), "native CGB loads optional boot image");
    auto *ram = static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(ram != nullptr, "CGB boot path exposes cartridge SRAM");
    retro_run();
    expect(observedFrame[80 * WIDTH + 80] == 0x0000FF && ram[0x20] == 0x80,
           "second CGB boot window renders a blue frame and reads cartridge header hole");
    std::vector<uint8_t> saved(retro_serialize_size());
    expect(retro_serialize(saved.data(), saved.size()), "serialize while CGB BIOS remains mapped");
    for (unsigned n = 0; n < 12; ++n) retro_run();
    expect(ram[0] == 0x11 && observedFrame[80 * WIDTH + 80] == 0xFF0000,
           "FF50 handoff reaches native CGB game startup and red gameplay");
    const auto frame = observedFrame; const auto audio = latestAudio;
    std::vector<uint8_t> future(saved.size());
    expect(retro_serialize(future.data(), future.size()), "serialize post-CGB-boot game state");
    expect(retro_unserialize(saved.data(), saved.size()), "restore mapped CGB BIOS checkpoint");
    for (unsigned n = 0; n < 12; ++n) retro_run();
    std::vector<uint8_t> replay(saved.size());
    expect(retro_serialize(replay.data(), replay.size()), "serialize boot handoff replay");
    expect(frame == observedFrame && audio == latestAudio && future == replay,
           "mapped CGB boot restore reproduces handoff, video, PCM and complete state");
    std::filesystem::remove(directory / "cgb_boot.bin");
    retro_reset(); retro_run();
    expect(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) == ram && observedFrame[80 * WIDTH + 80] == 0x0000FF,
           "CGB reset reuses selected boot image and stable SRAM without rereading removed file");
    retro_unload_game();
    writeBoot("gbc_bios.bin", boot);
    expect(retro_load_game(&game), "CGB BIOS fallback filename loads");
    expect(retro_unserialize(saved.data(), saved.size()), "same CGB BIOS bytes under alias preserve snapshot identity");
    retro_unload_game();
    std::filesystem::remove(directory / "gbc_bios.bin");
    expect(retro_load_game(&game), "missing CGB BIOS uses fast startup");
    std::vector<uint8_t> fastBefore(saved.size()), fastAfter(saved.size());
    expect(retro_serialize(fastBefore.data(), fastBefore.size()), "save no-BIOS CGB state");
    expect(!retro_unserialize(saved.data(), saved.size()), "mapped CGB BIOS snapshot rejects different boot configuration");
    expect(retro_serialize(fastAfter.data(), fastAfter.size()) && fastBefore == fastAfter,
           "CGB BIOS identity mismatch leaves running fast-start game intact");
    retro_unload_game();
    writeBoot("gbc_bios.bin", boot);
    writeBoot("cgb_boot.bin", std::vector<uint8_t>(0x800, 0));
    logMessages.clear();
    expect(!retro_load_game(&game), "malformed preferred CGB BIOS does not silently fall through to a valid alias");
    expect(!logMessages.empty() && logLevels.back() == RETRO_LOG_ERROR &&
               logMessages.back().find("2304 bytes") != std::string::npos,
           "CGB BIOS size failure reaches frontend logging boundary");
    systemDirectory.clear(); std::filesystem::remove_all(directory);
    std::cout << "CGB optional BIOS rendering, handoff, state replay, reset and diagnostics passed\n";
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

std::vector<uint8_t> makeInputPaletteRom(uint16_t delay, bool vblankOnly) {
    // Original ROM, no external assets. JOYP Up selects white, release black.
    std::vector<uint8_t> rom(0x8000, 0);
    rom[0x100] = 0xC3;
    rom[0x101] = 0x50;
    rom[0x102] = 0x01;
    std::vector<uint8_t> program = {
        0xF3,                         // DI
        0xF0, 0x44, 0xFE, 0x90, 0x38, 0xFA, // wait for VBlank
        0xAF, 0xE0, 0x40,             // disable LCD
        0x3E, 0xFF, 0xE0, 0x47,       // neutral palette: all black
        0x3E, 0x20, 0xE0, 0x00,       // JOYP: select directions
        0x01, static_cast<uint8_t>(delay), static_cast<uint8_t>(delay >> 8),
        0x0B, 0x78, 0xB1, 0x20, 0xFB, // delay LCD startup
        0x3E, 0x91, 0xE0, 0x40,       // enable LCD + BG
    };
    if (vblankOnly) {
        const std::vector<uint8_t> wait = {
            0xF0, 0x44, 0xFE, 0x90, 0x30, 0xFA, // wait for visible frame
            0xF0, 0x44, 0xFE, 0x90, 0x38, 0xFA, // wait for VBlank
        };
        program.insert(program.end(), wait.begin(), wait.end());
    }
    const std::vector<uint8_t> sample = {
        0xF0, 0x00, 0xE6, 0x04,       // read Up (active low)
        0x28, 0x04,                   // JR Z, pressed
        0x3E, 0xFF, 0x18, 0x01,       // neutral: black; JR write
        0xAF,                         // pressed: white
        0xE0, 0x47,                   // write BGP
        0x18, static_cast<uint8_t>(vblankOnly ? -27 : -15),
    };
    program.insert(program.end(), sample.begin(), sample.end());
    std::copy(program.begin(), program.end(), rom.begin() + 0x150);
    return rom;
}

void testLibretroInputPulseAcrossLcdPhases() {
    for (bool xrgb : {true, false}) {
        retro_unload_game();
        retro_deinit();
        frontendAcceptsXrgb8888 = xrgb;
        retro_init();
        latchUpOnPoll = true;
        for (bool vblankOnly : {false, true}) {
            std::array<unsigned, 3> pressHistogram{}, releaseHistogram{};
            for (uint16_t delay : {1, 127, 512, 1024, 2047}) {
                const auto rom = makeInputPaletteRom(delay, vblankOnly);
                retro_game_info game{};
                game.data = rom.data();
                game.size = rom.size();
                expect(retro_load_game(&game), "input palette probe loads");
                logLevels.clear();
                pendingUpPressed = false;
                for (unsigned i = 0; i < 10; ++i) retro_run();
                expect(std::all_of(observedFrame.begin(), observedFrame.end(),
                                  [](uint32_t p) { return p == 0; }),
                       "neutral input probe settles to black");
                unsigned press = 0, release = 0;
                // A single poll of Up, immediately followed by neutral polls.
                for (unsigned call = 1; call <= 4; ++call) {
                    pendingUpPressed = call == 1;
                    retro_run();
                    const bool white = std::any_of(observedFrame.begin(), observedFrame.end(),
                                                  [](uint32_t p) { return p != 0; });
                    if (!press && white) press = call;
                    if (press && !release && !white) release = call - 1;
                }
                expect(press > 0 && press <= 2, "one-poll input reaches video within two calls including press");
                expect(release > 0 && release <= 2, "input release reaches video within two calls including release");
                ++pressHistogram[press];
                ++releaseHistogram[release];
                expect(std::find(logLevels.begin(), logLevels.end(), RETRO_LOG_ERROR) == logLevels.end(),
                       "input probe executes without core errors");
                retro_unload_game();
            }
            std::cout << "Input palette response (" << (xrgb ? "XRGB8888" : "0RGB1555")
                      << ", " << (vblankOnly ? "VBlank" : "continuous")
                      << "): press calls 1/2=" << pressHistogram[1] << '/' << pressHistogram[2]
                      << ", release calls 1/2=" << releaseHistogram[1] << '/' << releaseHistogram[2] << '\n';
        }
        latchUpOnPoll = false;
        pendingUpPressed = upPressed = false;
    }
    retro_deinit();
    frontendAcceptsXrgb8888 = true;
    retro_init();
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

void testLibretroRepeatedRewindReplay() {
    const auto rom = readBinary(std::string(LOTUSGB_LIBRETRO_TEST_ASSET_DIR) + "/gb-pong/gb-pong.gb");
    retro_game_info game{}; game.data = rom.data(); game.size = rom.size();
    for (bool xrgb : {true, false}) {
        retro_deinit(); frontendAcceptsXrgb8888 = xrgb; retro_init();
        expect(retro_load_game(&game), "Pong loads for rewind protocol regression");
        upPressed = false;
        for (unsigned i = 0; i < 60; ++i) retro_run();
        const auto save = [] {
            std::vector<uint8_t> state(retro_serialize_size());
            expect(!state.empty() && retro_serialize(state.data(), state.size()), "rewind checkpoint saves");
            return state;
        };
        const auto input = [](unsigned frame) {
            return (frame >= 5 && frame < 9) || (frame >= 15 && frame < 19) ||
                   (frame >= 22 && frame < 25);
        };
        std::vector<std::vector<uint8_t>> history;
        std::vector<std::array<uint32_t, WIDTH * HEIGHT>> frames;
        std::vector<std::vector<int16_t>> pcm;
        for (unsigned frame = 0; frame < 30; ++frame) {
            history.push_back(save());
            upPressed = input(frame); retro_run();
            frames.push_back(observedFrame); pcm.push_back(latestAudio);
        }
        const auto future = save();
        for (unsigned target : {20, 10, 0, 25}) {
            expect(retro_unserialize(history[target].data(), history[target].size()),
                   "rewind restores a non-monotonic history checkpoint");
            for (unsigned frame = target; frame < 30; ++frame) {
                upPressed = input(frame); retro_run();
                expect(observedFrame == frames[frame] && latestAudio == pcm[frame],
                       "rewind/replay reproduces every video frame and PCM batch");
            }
            expect(save() == future, "repeated rewind/replay reaches the identical final state");
        }
        // Resume with new input instead of replaying recorded input.
        std::vector<std::array<uint32_t, WIDTH * HEIGHT>> neutral;
        upPressed = false;
        for (unsigned frame = 0; frame < 4; ++frame) { retro_run(); neutral.push_back(observedFrame); }
        expect(retro_unserialize(future.data(), future.size()), "restore before new post-rewind input");
        upPressed = true;
        bool paddleMoved = false;
        for (unsigned frame = 0; frame < 4; ++frame) {
            retro_run();
            for (unsigned y = 0; y < HEIGHT; ++y)
                for (unsigned x = 4; x < 12; ++x)
                    paddleMoved |= observedFrame[y * WIDTH + x] != neutral[frame][y * WIDTH + x];
        }
        expect(paddleMoved, "new directional input controls the paddle after repeated rewind");
        retro_unload_game();
    }
    retro_deinit(); frontendAcceptsXrgb8888 = true; upPressed = false; retro_init();
    std::cout << "Rewind protocol: 4 history restores/replays, exact video/PCM/state, "
                 "and resumed input passed in both pixel formats\n";
}

void testLibretroCgbColorInputStateAndReset() {
    const auto acid = readBinary(std::string(LOTUSGB_LIBRETRO_TEST_ASSET_DIR)+"/cgb-acid2/cgb-acid2.gbc");
    const auto expected = readBinary(std::string(LOTUSGB_LIBRETRO_TEST_ASSET_DIR)+"/cgb-acid2/reference-rgb555.bin");
    for (bool xrgb : {true,false}) {
        frontendAcceptsXrgb8888=xrgb;
        retro_deinit(); retro_init();
        buttonsPressed=0; upPressed=false;
        retro_game_info game{}; game.path="test.gbc"; game.data=acid.data(); game.size=acid.size();
        expect(retro_load_game(&game),"core loads CGB acid2 through .gbc content path");
        for(unsigned i=0;i<20;++i) retro_run();
        expect(expected.size()==observedFrame.size()*2,"CGB reference is a full color frame");
        for(size_t i=0;i<observedFrame.size();++i) {
            const unsigned c=expected[i*2]|(expected[i*2+1]<<8);
            const auto expand=[](unsigned v) { return (v<<3)|(v>>2); };
            const unsigned rgb=(expand(c&31)<<16)|(expand((c>>5)&31)<<8)|expand((c>>10)&31);
            expect(observedFrame[i]==rgb,"CGB acid2 pixels match in both negotiated output formats");
        }
        retro_unload_game();
        auto rom=LOTUSGB::Test::makeCgbExerciseRom();
        game.path="dual.gb"; game.data=rom.data(); game.size=rom.size();
        expect(retro_load_game(&game),"CGB header selects color hardware even with .gb extension");
        for(unsigned i=0;i<6;++i) retro_run();
        auto *ram=static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
        expect(ram && ram[0]==0x11 && ram[1]==0x33 && ram[2]==0xFE,"libretro executes CGB startup, DMA and speed switch");
        expect(observedFrame[80*WIDTH+80]==0xFF0000,"CGB neutral frame is red");
        auto save=[] {
            std::vector<uint8_t> state(retro_serialize_size());
            expect(!state.empty() && retro_serialize(state.data(),state.size()),"CGB libretro checkpoint serializes");
            return state;
        };
        const auto saved=save();
        std::vector<std::array<uint32_t,WIDTH*HEIGHT>> video;
        std::vector<std::vector<int16_t>> audio;
        for(unsigned i=0;i<12;++i) {
            buttonsPressed=i>=2 ? 1u<<RETRO_DEVICE_ID_JOYPAD_RIGHT : 0;
            retro_run(); video.push_back(observedFrame); audio.push_back(latestAudio);
        }
        expect(observedFrame[80*WIDTH+80]==0x0000FF,"RetroArch joypad input changes CGB output to blue");
        const auto future=save();
        for(unsigned replay=0;replay<3;++replay) {
            expect(retro_unserialize(saved.data(),saved.size()),"CGB rewind restores earlier checkpoint");
            expect(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)==ram,"CGB restore keeps SRAM address stable");
            for(unsigned i=0;i<12;++i) {
                buttonsPressed=i>=2 ? 1u<<RETRO_DEVICE_ID_JOYPAD_RIGHT : 0;
                retro_run();
                expect(observedFrame==video[i] && latestAudio==audio[i],"CGB rewind replays exact video and PCM");
            }
            expect(save()==future,"CGB repeated rewind reaches identical complete state");
        }
        ram[0x123]=0xA5; buttonsPressed=0;
        retro_reset();
        expect(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)==ram && ram[0x123]==0xA5,"CGB reset preserves SRAM contents and pointers");
        for(unsigned i=0;i<6;++i) retro_run();
        expect(ram[0]==0x11 && observedFrame[80*WIDTH+80]==0xFF0000,"CGB reset retains model, speed and color startup");
        retro_unload_game();
    }
    frontendAcceptsXrgb8888=true;
    retro_deinit(); retro_init();
    std::cout<<"CGB libretro: acid2 color, .gb/.gbc header detection, input, reset and exact state/rewind pass in both formats\n";
}

void testLibretroCgbMbc5HighBankGameplay() {
    constexpr size_t progress = 15 * 0x2000 + 0x10;
    for (bool xrgb : {true, false}) {
        retro_deinit();
        frontendAcceptsXrgb8888 = xrgb;
        retro_init();
        for (uint16_t bank : {uint16_t(0x100), uint16_t(0x1FF)}) {
            auto rom = LOTUSGB::Test::makeCgbMbc5HighBankExerciseRom(bank);
            retro_game_info game{};
            game.path = "original-cgb-mbc5-high-bank.gbc";
            game.data = rom.data(); game.size = rom.size();
            buttonsPressed = 0;
            expect(retro_load_game(&game), "load 8-MiB native CGB MBC5 game");
            auto *ram = static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
            expect(ram && retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0x20000,
                   "MBC5 exposes all 128 KiB of battery RAM");
            std::fill_n(ram, 0x20000, 0);
            // Distinct untouched bytes catch upper/lower SRAM aliasing.
            for (unsigned b = 0; b < 16; ++b) ram[b * 0x2000 + 123] = 0x80 + b;
            ram[progress] = 40;
            for (unsigned n = 0; n < 6; ++n) retro_run();
            expect(ram[0] == 0x11 && ram[progress] > 40 &&
                       ram[progress + 1] == static_cast<uint8_t>(bank) &&
                       ram[progress + 2] == 1,
                   "CGB CPU executes bank $100/$1FF and advances SRAM bank 15");
            expect(observedFrame[80 * WIDTH + 80] == 0xFF0000,
                   "high-bank CGB game renders its neutral scene");
            const auto save = [] {
                std::vector<uint8_t> bytes(retro_serialize_size());
                expect(retro_serialize(bytes.data(), bytes.size()), "serialize high-bank CGB game");
                return bytes;
            };
            const auto saved = save();
            const auto battery = std::vector<uint8_t>(ram, ram + 0x20000);
            const uint8_t before = ram[progress];
            buttonsPressed = 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
            std::vector<std::array<uint32_t, WIDTH * HEIGHT>> video;
            std::vector<std::vector<int16_t>> pcm;
            for (unsigned n = 0; n < 4; ++n) {
                retro_run(); video.push_back(observedFrame); pcm.push_back(latestAudio);
            }
            expect(ram[progress] > before && observedFrame[80 * WIDTH + 80] == 0x0000FF,
                   "high-bank code keeps running while CGB input changes the scene");
            const auto future = save();
            expect(retro_unserialize(saved.data(), saved.size()), "restore MBC5 ninth ROM bank bit");
            expect(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) == ram,
                   "MBC5 state restore preserves published battery pointer");
            for (unsigned n = 0; n < 4; ++n) {
                retro_run();
                expect(observedFrame == video[n] && latestAudio == pcm[n],
                       "MBC5 high-bank replay matches every video and PCM callback");
            }
            expect(save() == future, "MBC5 high-bank replay matches complete end state");
            retro_unload_game();
            expect(retro_load_game(&game), "reload MBC5 game for fresh battery import");
            ram = static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
            expect(ram && retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == battery.size(),
                   "fresh MBC5 instance exposes full battery import destination");
            std::copy(battery.begin(), battery.end(), ram);
            buttonsPressed = 0;
            for (unsigned n = 0; n < 6; ++n) retro_run();
            expect(ram[progress] > before && observedFrame[80 * WIDTH + 80] == 0xFF0000,
                   "fresh game resumes and advances imported upper-bank progress");
            for (unsigned b = 0; b < 16; ++b)
                expect(ram[b * 0x2000 + 123] == 0x80 + b,
                       "all sixteen SRAM banks retain independent battery bytes");
            retro_unload_game();
        }
    }
    retro_deinit(); frontendAcceptsXrgb8888 = true; buttonsPressed = 0; retro_init();
    std::cout << "CGB MBC5 high ROM/SRAM banks, input, battery resume and exact replay passed in both formats\n";
}

void testLibretroMbc30BatteryAndState() {
    for(size_t bytes : {size_t(0x200000),size_t(0x400000)}) {
        auto rom=LOTUSGB::Test::makeCgbMbc30ExerciseRom();rom.resize(bytes);
        rom[0x148]=bytes==0x200000?6:7;
        retro_game_info game{};game.path="original-mbc30.gbc";game.data=rom.data();game.size=rom.size();
        buttonsPressed=0;
        expect(retro_load_game(&game),"libretro accepts 2/4-MiB MBC30 with 64-KiB SRAM");
        for(unsigned frame=0;frame<6;++frame)retro_run();
        auto *ram=static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
        auto *rtc=static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_RTC));
        expect(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM)==65536 && retro_get_memory_size(RETRO_MEMORY_RTC)==24 && ram && rtc,
               "frontend receives full MBC30 SRAM and separate RTC storage");
        expect(ram[0]==0x11 && ram[4]==(bytes==0x400000?0x80:0) && ram[7*0x2000]==0x77,
               "CPU accesses MBC30 ROM bank $80 and SRAM bank 7 without aliasing classic MBC3");
        ram[6*0x2000+123]=0x6C;
        const std::vector<uint8_t> battery(ram,ram+65536),clock(rtc,rtc+24);
        std::vector<uint8_t> saved(retro_serialize_size());
        expect(retro_serialize(saved.data(),saved.size()),"MBC30 bank $80 snapshot serializes");
        buttonsPressed=1u<<RETRO_DEVICE_ID_JOYPAD_RIGHT;
        for(unsigned frame=0;frame<4;++frame)retro_run();
        const auto frame=observedFrame;const auto pcm=latestAudio;
        std::vector<uint8_t> future(saved.size());expect(retro_serialize(future.data(),future.size()),"MBC30 future state saves");
        expect(retro_unserialize(saved.data(),saved.size()),"MBC30 snapshot restores high ROM bank");
        expect(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)==ram && retro_get_memory_data(RETRO_MEMORY_RTC)==rtc,
               "MBC30 restore preserves SRAM and RTC pointers");
        for(unsigned n=0;n<4;++n)retro_run();
        std::vector<uint8_t> replay(saved.size());expect(retro_serialize(replay.data(),replay.size()),"MBC30 replay state saves");
        expect(frame==observedFrame && pcm==latestAudio && replay==future,"MBC30 replay has exact color, audio and hardware state");
        retro_unload_game();
        expect(retro_load_game(&game),"MBC30 reloads for frontend battery import");
        ram=static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));rtc=static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_RTC));
        std::copy(battery.begin(),battery.end(),ram);std::copy(clock.begin(),clock.end(),rtc);
        buttonsPressed=0;for(unsigned n=0;n<6;++n)retro_run();
        expect(ram[6*0x2000+123]==0x6C,"MBC30 upper-bank battery data survives a fresh content load");
        expect(retro_unserialize(saved.data(),saved.size()),"MBC30 snapshot loads after fresh content load");
        retro_unload_game();
    }
    buttonsPressed=0;
    std::cout<<"MBC30 libretro ROM/RAM/RTC, battery reload and state replay passed\n";
}

void runLibretroCgbBatteryGameProgressReload(std::vector<uint8_t> rom) {
    retro_game_info game{};
    game.path = "cgb-battery-game.gb";
    game.data = rom.data();
    game.size = rom.size();

    frontendAcceptsXrgb8888 = true;
    buttonsPressed = 0;
    upPressed = false;
    latchUpOnPoll = false;
    expect(retro_load_game(&game),
           "core loads a CGB-compatible battery game fixture");
    for (unsigned frame = 0; frame < 6; ++frame)
        retro_run();

    const size_t saveRamSize = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    expect(saveRamSize == 0x2000,
           "CGB game exposes its declared 8-KiB battery RAM");
    auto *saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(saveRam != nullptr && saveRam[0] == 0x11 &&
               saveRam[1] == 0x33 && saveRam[2] == 0xFE,
           "CGB game startup writes its expected cartridge progress signature");
    expect(observedFrame[80 * WIDTH + 80] == 0xFF0000,
           "CGB game renders its neutral color scene");
    const uint8_t initialProgress = saveRam[0x10];

    buttonsPressed = 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    for (unsigned frame = 0; frame < 4; ++frame)
        retro_run();
    expect(observedFrame[80 * WIDTH + 80] == 0x0000FF,
           "CGB game input changes the visible color scene");
    expect(saveRam[0x10] > initialProgress,
           "CGB game advances progress in battery RAM while playing");

    const uint8_t savedProgress = saveRam[0x10];
    const std::vector<uint8_t> savedRam(saveRam, saveRam + saveRamSize);
    retro_unload_game();

    buttonsPressed = 0;
    expect(retro_load_game(&game),
           "frontend can start a fresh CGB content instance");
    expect(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == saveRamSize,
           "fresh CGB instance exposes the same battery RAM size");
    saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(saveRam != nullptr,
           "fresh CGB instance exposes its battery RAM buffer");
    std::copy(savedRam.begin(), savedRam.end(), saveRam);
    expect(saveRam[0x10] == savedProgress,
           "frontend restores CGB game progress before execution");
    for (unsigned frame = 0; frame < 6; ++frame)
        retro_run();
    expect(saveRam[0x10] > savedProgress,
           "reloaded CGB game reads and advances the restored progress");
    expect(observedFrame[80 * WIDTH + 80] == 0xFF0000,
           "reloaded CGB game returns to its neutral color scene");

    buttonsPressed = 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    for (unsigned frame = 0; frame < 4; ++frame)
        retro_run();
    expect(observedFrame[80 * WIDTH + 80] == 0x0000FF,
           "reloaded CGB game still responds to directional input");
    retro_unload_game();
    buttonsPressed = 0;
}

void testLibretroCgbMbc1AndMbc5BatteryGameProgressReload() {
    runLibretroCgbBatteryGameProgressReload(
        LOTUSGB::Test::makeCgbMbc1BatteryExerciseRom());
    runLibretroCgbBatteryGameProgressReload(
        LOTUSGB::Test::makeCgbMbc5BatteryExerciseRom());
    std::cout << "CGB MBC1/MBC5 gameplay and battery SRAM reload passed\n";
}

void testLibretroCgbMbc3RtcGameplayAndBatteryReload() {
    auto rom = LOTUSGB::Test::makeCgbMbc3RtcExerciseRom();
    retro_game_info game{};
    game.path = "cgb-mbc3-rtc.gbc";
    game.data = rom.data();
    game.size = rom.size();

    buttonsPressed = 0;
    expect(retro_load_game(&game), "core loads a CGB-compatible MBC3+RTC game");
    expect(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0x8000 &&
               retro_get_memory_size(RETRO_MEMORY_RTC) == 24,
           "CGB MBC3 game exposes separate 32-KiB SRAM and RTC blocks");

    auto *rtc = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_RTC));
    expect(rtc != nullptr, "CGB MBC3 game exposes importable RTC data");
    std::fill(rtc, rtc + 24, 0);
    rtc[0] = 'L'; rtc[1] = 'G'; rtc[2] = 'R'; rtc[3] = 'T';
    rtc[4] = 1;       // Versioned persistence payload.
    rtc[5] = 37;      // Live seconds.
    rtc[9] = 0x40;    // Halt the clock so the test is independent of wall time.
    rtc[10] = 37;     // Latched seconds returned by the guest-visible register.
    rtc[14] = 0x40;   // Latched day-high register also records the halt bit.
    rtc[15] = 0xFF;   // Permit the guest's 0 -> 1 latch edge.

    for (unsigned frame = 0; frame < 2; ++frame)
        retro_run();
    auto *saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(saveRam != nullptr && saveRam[0x30] == 38,
           "CGB guest reads, increments and writes the MBC3 RTC seconds register");
    expect(rtc[5] == 38 && rtc[10] == 38 && (rtc[9] & 0x40) != 0,
           "RTC live and latched state reflect the guest register write");

    const std::vector<uint8_t> savedRam(saveRam, saveRam + 0x8000);
    const std::vector<uint8_t> savedRtc(rtc, rtc + 24);
    retro_unload_game();

    expect(retro_load_game(&game),
           "frontend can reload the CGB MBC3+RTC content instance");
    saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    rtc = static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_RTC));
    expect(saveRam != nullptr && rtc != nullptr,
           "fresh CGB MBC3 instance exposes both importable battery blocks");
    std::copy(savedRam.begin(), savedRam.end(), saveRam);
    std::copy(savedRtc.begin(), savedRtc.end(), rtc);
    for (unsigned frame = 0; frame < 2; ++frame)
        retro_run();
    expect(saveRam[0x30] == 39 && rtc[5] == 39 && rtc[10] == 39,
           "reloaded CGB guest resumes from imported MBC3 RTC and SRAM state");
    retro_unload_game();
    std::cout << "CGB MBC3 RTC guest registers and frontend battery reload passed\n";
}

void testLibretroCgbRtcOfflineProgression() {
    auto rom = LOTUSGB::Test::makeCgbRtcOfflineResumeRom();
    retro_game_info game{};
    game.path = "cgb-rtc-offline.gbc";
    game.data = rom.data();
    game.size = rom.size();
    buttonsPressed = 0;
    expect(retro_load_game(&game), "load running-RTC offline-resume fixture");
    auto *rtc = static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_RTC));
    expect(rtc && retro_get_memory_size(RETRO_MEMORY_RTC) == 24,
           "running RTC exposes the versioned battery block");
    std::fill_n(rtc, 24, 0);
    std::copy_n("LGRT", 4, rtc);
    rtc[4] = 1;
    rtc[5] = 58; rtc[6] = 59; rtc[7] = 23; rtc[8] = 255; rtc[9] = 1;
    rtc[15] = 0xFF;
    const auto unixNow = [] {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    };
    const uint64_t savedAt = unixNow() - 125;
    for (unsigned i = 0; i < 8; ++i)
        rtc[16 + i] = static_cast<uint8_t>(savedAt >> (8 * i));
    for (unsigned i = 0; i < 2; ++i) retro_run();
    auto *ram = static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(ram != nullptr, "offline-resume fixture exposes SRAM observations");
    uint64_t frozenAt = 0;
    for (unsigned i = 0; i < 8; ++i)
        frozenAt |= static_cast<uint64_t>(rtc[16 + i]) << (8 * i);
    expect(frozenAt >= savedAt + 125 && frozenAt <= unixNow(),
           "guest halt records the current host time after offline import");
    // Bound elapsed time by host observations rather than requiring execution
    // within one wall-clock second. No sleeps or arbitrary timing tolerance.
    std::array<uint8_t, 5> expected{};
    std::copy_n(ram + 0x30, 5, expected.begin());
    const uint64_t observed = expected[0] + 60ULL * expected[1] +
        3600ULL * expected[2] + 86400ULL *
        (expected[3] | (static_cast<unsigned>(expected[4] & 1) << 8));
    expect(expected[0] < 60 && expected[1] < 60 && expected[2] < 24 &&
               expected[4] == 0xC0 && observed >= 123 &&
               observed <= frozenAt - savedAt - 2,
           "guest sees offline seconds/minutes/day rollover and sticky carry");
    expect(std::equal(expected.begin(), expected.end(), rtc + 5) &&
               std::equal(expected.begin(), expected.end(), rtc + 10),
           "battery live/latched clock matches all five guest observations");
    const std::vector<uint8_t> battery(ram, ram + 0x8000), clock(rtc, rtc + 24);
    retro_unload_game();
    expect(retro_load_game(&game), "reload offline-resume content in a fresh instance");
    ram = static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    rtc = static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_RTC));
    expect(ram && rtc, "fresh instance exposes both battery import destinations");
    std::copy(battery.begin(), battery.end(), ram);
    std::copy(clock.begin(), clock.end(), rtc);
    std::fill_n(ram + 0x30, 5, 0); // Require new guest reads, not stale SRAM.
    for (unsigned i = 0; i < 2; ++i) retro_run();
    expect(std::equal(expected.begin(), expected.end(), ram + 0x30),
           "fresh guest reads restored halted clock without adding offline time twice");
    retro_unload_game();
    std::cout << "CGB RTC offline progression, rollover and battery resume passed\n";
}

void measureTobuInput(const std::string &romPath, const std::filesystem::path &output, bool deluxe = false) {
    const unsigned firstCheckpoint = deluxe ? 1780 : 1840;
    const auto rom = readBinary(romPath);
    expect(rom.size() == 0x40000, "Tobu measurement expects the pinned 256 KiB ROM");
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    expect(retro_load_game(&game), "Tobu Tobu Girl loads for input measurement");
    logLevels.clear();
    const auto button = [](unsigned id) { return 1u << id; };
    for (unsigned frame = 0; frame < firstCheckpoint; ++frame) {
        buttonsPressed = 0;
        if (deluxe) {
            if ((frame >= 900 && frame < 904) || (frame >= 1200 && frame < 1204) || (frame >= 1500 && frame < 1504))
                buttonsPressed = button(RETRO_DEVICE_ID_JOYPAD_START);
        } else {
            if (frame >= 180 && frame < 1445 && frame % 180 < 5)
                buttonsPressed = button(RETRO_DEVICE_ID_JOYPAD_START);
            if ((frame >= 1620 && frame < 1625) || (frame >= 1800 && frame < 1805))
                buttonsPressed = button(RETRO_DEVICE_ID_JOYPAD_A);
        }
        retro_run();
    }
    std::filesystem::create_directories(output);
    const auto writeFrame = [&](const std::string &name,
                                const std::array<uint32_t, WIDTH * HEIGHT> &frame) {
        std::ofstream file(output / (name + ".ppm"), std::ios::binary);
        file << "P6\n160 144\n255\n";
        for (auto pixel : frame) {
            const char rgb[] = {static_cast<char>(pixel >> 16),
                                static_cast<char>(pixel >> 8), static_cast<char>(pixel)};
            file.write(rgb, 3);
        }
        expect(static_cast<bool>(file), "Tobu evidence frame writes successfully");
    };
    const auto differs = [](const auto &a, const auto &b) {
        // Central playfield around the character; exclude the right-hand HUD.
        for (unsigned y = 40; y < 136; ++y)
            for (unsigned x = 40; x < 120; ++x)
                if (a[y * WIDTH + x] != b[y * WIDTH + x]) return true;
        return false;
    };
    struct Case { const char *name; unsigned mask; };
    const std::array<Case, 6> cases{{
        {"left", button(RETRO_DEVICE_ID_JOYPAD_LEFT)},
        {"right", button(RETRO_DEVICE_ID_JOYPAD_RIGHT)},
        {"a-no-direction", button(RETRO_DEVICE_ID_JOYPAD_A)},
        {"flap-b", button(RETRO_DEVICE_ID_JOYPAD_B)},
        {"right-dash", button(RETRO_DEVICE_ID_JOYPAD_RIGHT) | button(RETRO_DEVICE_ID_JOYPAD_A)},
        {"right-flap", button(RETRO_DEVICE_ID_JOYPAD_RIGHT) | button(RETRO_DEVICE_ID_JOYPAD_B)},
    }};
    constexpr unsigned WINDOW = 8, SAMPLES = 12;
    using Sequence = std::array<std::array<uint32_t, WIDTH * HEIGHT>, WINDOW>;
    // Heap storage avoids multi-megabyte test-stack allocations.
    auto neutral = std::make_unique<Sequence>();
    auto pulse = std::make_unique<Sequence>();
    auto held = std::make_unique<Sequence>();
    auto rightPulse = std::make_unique<Sequence>();
    auto leftPulse = std::make_unique<Sequence>();
    std::ofstream report(output / "measurements.json");
    report << "{\"first_checkpoint\":" << firstCheckpoint << ",\"samples\":12,\"window\":8,"
              "\"roi\":[40,40,120,136],\"step_mode\":\""
#ifndef LOTUSGB_TEST_FIXED_FRAME_STEP
           << "vblank-presentation"
#else
           << "fixed-tcycles"
#endif
           << "\",\"results\":[\n";
    bool first = true;
    for (unsigned sample = 0; sample < SAMPLES; ++sample) {
        std::vector<uint8_t> saved(retro_serialize_size());
        expect(!saved.empty() && retro_serialize(saved.data(), saved.size()), "save Tobu checkpoint");
        if (sample == 0) {
            writeFrame("before", observedFrame);
            if (deluxe) {
                std::ofstream stateFile(output / "gameplay.state",std::ios::binary);
                stateFile.write(reinterpret_cast<const char *>(saved.data()),saved.size());
                expect(static_cast<bool>(stateFile),"Deluxe gameplay state writes for a fresh frontend session");
            }
        }
        std::vector<std::vector<int16_t>> branchPcm;
        std::vector<uint8_t> branchState(saved.size());
        const auto runBranch = [&](unsigned mask, bool onePoll, Sequence &frames) {
            branchPcm.clear();
            expect(retro_unserialize(saved.data(), saved.size()), "restore Tobu comparison checkpoint");
            for (unsigned frame = 0; frame < WINDOW; ++frame) {
                buttonsPressed = !onePoll || frame == 0 ? mask : 0;
                const auto polls = inputPollCount, videos = videoRefreshCount;
                retro_run();
                expect(inputPollCount == polls + 1 && videoRefreshCount == videos + 1,
                       "Tobu comparison gets one input poll and video callback per run");
                frames[frame] = observedFrame;
                branchPcm.push_back(latestAudio);
            }
            expect(retro_serialize(branchState.data(),branchState.size()),"Tobu branch end state saves");
        };
        runBranch(0, false, *neutral);
        const auto neutralPcm=branchPcm;
        const auto neutralState=branchState;
        // Repeated neutral replay must be identical before attributing differences to input.
        runBranch(0, false, *pulse);
        expect(*neutral == *pulse && neutralPcm==branchPcm && neutralState==branchState,
               "Tobu neutral replay reproduces exact frames, PCM and full state");
        for (const auto &item : cases) {
            runBranch(item.mask, true, *pulse);
            runBranch(item.mask, false, *held);
            if (item.mask == button(RETRO_DEVICE_ID_JOYPAD_LEFT)) *leftPulse = *pulse;
            if (item.mask == button(RETRO_DEVICE_ID_JOYPAD_RIGHT)) {
                *rightPulse = *pulse;
                expect(*leftPulse != *rightPulse, "Tobu Left and Right produce distinct responses");
            }
            unsigned press = 0, tap = 0, release = 0;
            unsigned compoundEffect = 0;
            for (unsigned frame = 0; frame < WINDOW; ++frame) {
                if (!press && differs((*held)[frame], (*neutral)[frame])) press = frame + 1;
                if (!tap && differs((*pulse)[frame], (*neutral)[frame])) tap = frame + 1;
                if (frame > 0 && !release && differs((*pulse)[frame], (*held)[frame])) release = frame;
                if ((item.mask & button(RETRO_DEVICE_ID_JOYPAD_RIGHT)) &&
                    item.mask != button(RETRO_DEVICE_ID_JOYPAD_RIGHT) &&
                    !compoundEffect && differs((*pulse)[frame], (*rightPulse)[frame]))
                    compoundEffect = frame + 1;
                if (sample == 0) {
                    writeFrame(std::string(item.name) + "-pulse-" + std::to_string(frame + 1), (*pulse)[frame]);
                    writeFrame(std::string(item.name) + "-held-" + std::to_string(frame + 1), (*held)[frame]);
                }
            }
            const bool directionalOrFlap = item.mask != button(RETRO_DEVICE_ID_JOYPAD_A);
            if (directionalOrFlap && (!press || !tap))
                std::cerr << "Tobu checkpoint=" << firstCheckpoint + sample << " action=" << item.name
                          << " held=" << press << " pulse=" << tap << '\n';
            // A without a direction is diagnostic: do not equate absence of a
            // directionless dash with lost input. Direction+A is tested below.
            expect(!directionalOrFlap || (press > 0 && tap > 0),
                   "Tobu movement, flap and directional dash visibly respond to held and one-poll input");
            if ((item.mask & button(RETRO_DEVICE_ID_JOYPAD_RIGHT)) &&
                item.mask != button(RETRO_DEVICE_ID_JOYPAD_RIGHT))
                expect(compoundEffect > 0, "Tobu direction plus A/B differs from direction alone");
            if (!first) report << ",\n";
            first = false;
            report << "{\"checkpoint\":" << firstCheckpoint + sample << ",\"action\":\"" << item.name
                   << "\",\"held_press_call\":" << press << ",\"pulse_press_call\":" << tap
                   << ",\"release_difference_call\":" << release
                   << ",\"compound_effect_call\":" << compoundEffect << '}';
        }
        if (sample == 0)
            for (unsigned frame = 0; frame < WINDOW; ++frame)
                writeFrame("neutral-" + std::to_string(frame + 1), (*neutral)[frame]);
        expect(retro_unserialize(saved.data(), saved.size()), "restore neutral Tobu timeline");
        buttonsPressed = 0;
        retro_run();
    }
    expect(std::find(logLevels.begin(), logLevels.end(), RETRO_LOG_ERROR) == logLevels.end(),
           "Tobu input measurement reports no core errors");
    report << "\n]}\n";
    report.close();
    expect(static_cast<bool>(report), "Tobu measurement report writes successfully");
    std::cout << "Tobu input measurement: 12 checkpoints x 6 actions; neutral replay, "
                 "held input and one-poll response checks passed\n";
}

void testLibretroLinkSubsystem() {
    expect(linkSubsystem && std::string(linkSubsystem->ident) == "gb_link_2p" &&
           linkSubsystem->num_roms == 2, "core advertises a two-ROM link subsystem");
    auto serialRom = [](uint8_t value, uint8_t control) {
        std::vector<uint8_t> rom(0x8000);
        rom[0x147] = 0x03;
        rom[0x149] = 0x02;
        const uint8_t code[] = {
            0x3E, 0x0A, 0xEA, 0, 0,   // Enable SRAM.
            0x3E, value, 0xE0, 0x01,   // Set SB.
            0x3E, control, 0xE0, 0x02, // Start the transfer.
            0xF0, 0x02, 0xCB, 0x7F,   // Wait until SC bit 7 clears.
            0x20, 0xFA,
            0xF0, 0x01, 0xEA, 0, 0xA0, // Record received SB in SRAM.
            0x18, 0xFE,
        };
        std::copy(std::begin(code), std::end(code), rom.begin() + 0x100);
        return rom;
    };
    const auto first = serialRom(0xA5, 0x81);
    const auto second = serialRom(0x3C, 0x80);
    const retro_game_info games[] = {
        {"same.gb", first.data(), first.size(), nullptr},
        {"same.gb", second.data(), second.size(), nullptr},
    };
    const auto &firstSave = linkSubsystem->roms[0].memory[0];
    const auto &secondSave = linkSubsystem->roms[1].memory[0];
    expect(std::string(firstSave.extension) != secondSave.extension,
           "identical content basenames have separate save paths");
    expect(!retro_load_game_special(linkSubsystem->id, games, 1),
           "link requires both ROMs");
    expect(retro_load_game_special(linkSubsystem->id, games, 2), "both link ROMs load");
    auto *firstRam = static_cast<uint8_t *>(retro_get_memory_data(firstSave.type));
    auto *secondRam = static_cast<uint8_t *>(retro_get_memory_data(secondSave.type));
    expect(firstRam && secondRam && firstRam != secondRam &&
           retro_get_memory_size(firstSave.type) == 8192 &&
           retro_get_memory_size(secondSave.type) == 8192,
           "both battery regions are independently exposed");
    expect(retro_serialize_size() == 0, "single-system snapshots are disabled during local link");
    retro_set_video_refresh([](const void *data, unsigned width, unsigned height, size_t pitch) {
        expect(data && width == 2 * WIDTH && height == HEIGHT &&
               pitch == width * sizeof(uint32_t), "link outputs two screens side by side");
    });
    retro_run();
    expect(firstRam[0] == 0x3C && secondRam[0] == 0xA5, "both CPUs store the peer's byte");
    retro_reset();
    expect(firstRam == retro_get_memory_data(firstSave.type) &&
           secondRam == retro_get_memory_data(secondSave.type),
           "link reset preserves both frontend save pointers");
    firstRam[0] = secondRam[0] = 0;
    retro_run();
    expect(firstRam[0] == 0x3C && secondRam[0] == 0xA5, "link is rewired after reset");
    retro_unload_game();
    expect(retro_get_memory_data(secondSave.type) == nullptr, "link unload releases peer memory");
    retro_set_video_refresh(videoRefreshCallback);
}

void testLibretroNetpacketSerialExchange() {
    expect(netpacketInterface && netpacketInterface->start &&
           netpacketInterface->receive && netpacketInterface->stop &&
           netpacketInterface->poll && netpacketInterface->protocol_version &&
           std::string(netpacketInterface->protocol_version) == "LotusGB-serial-bit-v1",
           "core registers a versioned RetroArch Netpacket interface");
    std::vector<uint8_t> rom(0x8000);
    rom[0x147] = 0x03;
    rom[0x149] = 0x02;
    const uint8_t program[] = {
        0x3E, 0x0A, 0xEA, 0, 0,       // Enable SRAM.
        0x3E, 0, 0xEA, 0, 0xA0,      // Clear the received-byte location.
        0x3E, 0xA5, 0xE0, 1,          // Load SB.
        0x3E, 0x81, 0xE0, 2,          // Start internal-clock transfer.
        0xF0, 2, 0xCB, 0x7F, 0x20, 0xFA,
        0xF0, 1, 0xEA, 0, 0xA0,       // Store received byte.
        0x18, 0xFE,
    };
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x100);
    const retro_game_info game{"netpacket.gb", rom.data(), rom.size(), nullptr};
    expect(retro_load_game(&game), "single-ROM link game loads for Netpacket");
    mockSerialRequests = 0;
    netpacketInterface->start(1, mockNetpacketSend, mockNetpacketPollReceive);
    retro_run();
    auto *saveRam = static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(mockSerialRequests == 8 && saveRam && saveRam[0] == 0,
           "eight reliable Netpacket edges deliver the peer's byte to the game");
    netpacketInterface->stop();
    retro_unload_game();
}

void testLibretroNetpacketDisconnectFaultsCore() {
    std::vector<uint8_t> rom(0x8000);
    rom[0x147] = 0x03;
    rom[0x149] = 0x02;
    const uint8_t program[] = {
        0x3E, 0x0A, 0xEA, 0, 0,       // Enable SRAM.
        0x3E, 0xA5, 0xE0, 1,          // Load SB.
        0x3E, 0x81, 0xE0, 2,          // Start internal-clock transfer.
        0xF0, 2, 0xCB, 0x7F, 0x20, 0xFA,
        0xF0, 1, 0xEA, 0, 0xA0,
        0x18, 0xFE,
    };
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x100);
    const retro_game_info game{"netpacket-disconnect.gb", rom.data(),
                               rom.size(), nullptr};
    expect(retro_load_game(&game), "disconnect test serial ROM loads");
    logMessages.clear();
    logLevels.clear();
    mockSerialRequests = 0;
    mockDisconnectPolls = 0;
    netpacketInterface->start(0, mockNetpacketSendWithoutReply,
                              mockNetpacketDisconnectDuringPoll);
    expect(netpacketInterface->connected(1),
           "disconnect test host accepts its peer");

    retro_run();
    const bool loggedFault = std::any_of(
        logMessages.begin(), logMessages.end(), [](const std::string &message) {
            return message.find("Link peer disconnected during serial transfer") !=
                   std::string::npos;
        });
    expect(mockSerialRequests == 1 && mockDisconnectPolls == 1 && loggedFault,
           "peer disconnect faults the active transfer instead of inventing incoming bits");
    std::vector<uint8_t> state(retro_serialize_size());
    expect(!state.empty() && !retro_serialize(state.data(), state.size()),
           "a disconnected link core refuses further snapshots after its execution fault");

    netpacketInterface->stop();
    retro_unload_game();
}

void testLibretroNetpacketTimeoutFaultsCore() {
    std::vector<uint8_t> rom(0x8000);
    rom[0x147] = 0x03;
    rom[0x149] = 0x02;
    const uint8_t program[] = {
        0x3E, 0x0A, 0xEA, 0, 0,       // Enable SRAM.
        0x3E, 0xA5, 0xE0, 1,          // Load SB.
        0x3E, 0x81, 0xE0, 2,          // Start internal-clock transfer.
        0xF0, 2, 0xCB, 0x7F, 0x20, 0xFA,
        0xF0, 1, 0xEA, 0, 0xA0,
        0x18, 0xFE,
    };
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x100);
    const retro_game_info game{"netpacket-timeout.gb", rom.data(),
                               rom.size(), nullptr};
    expect(retro_load_game(&game), "timeout test serial ROM loads");
    logMessages.clear();
    logLevels.clear();
    mockSerialRequests = 0;
    netpacketInterface->start(1, mockNetpacketSendWithoutReply,
                              mockNetpacketPollReceive);

    retro_run();
    const bool loggedTimeout = std::any_of(
        logMessages.begin(), logMessages.end(), [](const std::string &message) {
            return message.find("Timed out waiting for a serial link bit") !=
                   std::string::npos;
        });
    expect(mockSerialRequests == 1 && loggedTimeout,
           "a missing link reply times out and faults instead of inventing incoming bits");
    std::vector<uint8_t> state(retro_serialize_size());
    expect(!state.empty() && !retro_serialize(state.data(), state.size()),
           "a timed-out link core refuses further snapshots after its execution fault");

    netpacketInterface->stop();
    retro_unload_game();
}

[[noreturn]] void runNetpacketPairEndpoint(unsigned player, int socket) {
    processNetpacketSocket = socket;
    processNetpacketPeerId = player == 0 ? 1 : 0;
    retro_deinit();
    retro_init();

    std::vector<uint8_t> rom(0x8000);
    rom[0x147] = 0x03;
    rom[0x149] = 0x02;
    const uint8_t outgoing = player == 0 ? 0xA5 : 0x3C;
    const uint8_t control = player == 0 ? 0x81 : 0x80;
    const uint8_t program[] = {
        0x3E, 0x0A, 0xEA, 0, 0,       // Enable SRAM.
        0x3E, 0, 0xEA, 0, 0xA0,      // Clear the received-byte location.
        0x3E, outgoing, 0xE0, 1,      // Load SB.
        0x3E, control, 0xE0, 2,       // Host clocks internally; client is external.
        0xF0, 2, 0xCB, 0x7F, 0x20, 0xFA,
        0xF0, 1, 0xEA, 0, 0xA0,       // Store received byte.
        0x18, 0xFE,
    };
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x100);
    const retro_game_info game{player == 0 ? "host.gb" : "client.gb",
                               rom.data(), rom.size(), nullptr};
    expect(retro_load_game(&game), "process-pair serial ROM loads");

    netpacketInterface->start(static_cast<uint16_t>(player),
                              processNetpacketSend,
                              processNetpacketPollReceive);
    if (player == 0)
        expect(netpacketInterface->connected(1),
               "process-pair host accepts its client");

    auto *saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    expect(saveRam != nullptr, "process-pair save RAM is available");
    const uint8_t expected = player == 0 ? 0x3C : 0xA5;
    const auto transferDeadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < transferDeadline &&
           saveRam[0] != expected) {
        retro_run();
        netpacketInterface->poll();
    }
    expect(saveRam && saveRam[0] == expected,
           "separate core processes exchange a complete serial byte");

    netpacketInterface->stop();
    retro_unload_game();
    retro_deinit();
    ::close(socket);
    _exit(EXIT_SUCCESS);
}

void testLibretroNetpacketTwoProcessExchange() {
    int sockets[2][2]{};
    expect(::socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets[0]) == 0,
           "host-to-relay Netpacket socket opens");
    if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets[1]) != 0) {
        ::close(sockets[0][0]);
        ::close(sockets[0][1]);
        expect(false, "relay-to-client Netpacket socket opens");
    }
#ifdef SO_NOSIGPIPE
    const int noSigPipe = 1;
    for (const auto &pair : sockets)
        for (const int socket : pair)
            expect(::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe,
                                sizeof(noSigPipe)) == 0,
                   "relay sockets suppress SIGPIPE on peer exit");
#endif

    const pid_t host = ::fork();
    expect(host >= 0, "fork host libretro process");
    if (host == 0) {
        ::close(sockets[0][0]);
        ::close(sockets[1][0]);
        ::close(sockets[1][1]);
        runNetpacketPairEndpoint(0, sockets[0][1]);
    }

    const pid_t client = ::fork();
    if (client < 0) {
        ::kill(host, SIGTERM);
        ::waitpid(host, nullptr, 0);
        expect(false, "fork client libretro process");
    }
    if (client == 0) {
        ::close(sockets[0][0]);
        ::close(sockets[0][1]);
        ::close(sockets[1][0]);
        runNetpacketPairEndpoint(1, sockets[1][1]);
    }

    ::close(sockets[0][1]);
    ::close(sockets[1][1]);
    const pid_t children[] = {host, client};
    bool finished[] = {false, false};
    int statuses[] = {-1, -1};
    bool relayEndpointClosed[] = {false, false};
    unsigned relayedPackets = 0;
    bool relaySucceeded = true;
    const auto relayStart = std::chrono::steady_clock::now();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(10);

    while ((!finished[0] || !finished[1]) &&
           std::chrono::steady_clock::now() < deadline) {
        struct pollfd descriptors[] = {
            {relayEndpointClosed[0] ? -1 : sockets[0][0], POLLIN, 0},
            {relayEndpointClosed[1] ? -1 : sockets[1][0], POLLIN, 0},
        };
        const int ready = ::poll(descriptors, 2, 10);
        if (ready < 0 && errno != EINTR) {
            relaySucceeded = false;
            break;
        }
        for (unsigned source = 0; source < 2; ++source) {
            if (!(descriptors[source].revents &
                  (POLLIN | POLLHUP | POLLERR)))
                continue;
            std::array<uint8_t, 64 * 1024> packet{};
            const ssize_t size = ::recv(sockets[source][0], packet.data(),
                                        packet.size(), 0);
            if (size == 0) {
                relayEndpointClosed[source] = true;
                if (relayedPackets != 16)
                    relaySucceeded = false;
                continue;
            }
            if (size < 0 && errno == EINTR)
                continue;
            if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            if (size <= 0) {
                if (relayedPackets == 16) {
                    // The completed client may close its datagram endpoint
                    // before the host process has returned from the final
                    // serial callback.
                    relayEndpointClosed[source] = true;
                    continue;
                }
                std::cerr << "Netpacket relay endpoint " << source
                          << " closed before all packets were forwarded; recv="
                          << size << " errno=" << errno << '\n';
                relaySucceeded = false;
                continue;
            }
            const auto packetDelay = std::chrono::milliseconds(
                25 + 25 * (relayedPackets % 3));
            std::this_thread::sleep_for(packetDelay);
            const unsigned target = 1 - source;
            const ssize_t written = ::send(sockets[target][0], packet.data(),
                                           static_cast<size_t>(size),
                                           processNetpacketSendFlags());
            if (written != size)
                relaySucceeded = false;
            else
                ++relayedPackets;
        }
        for (unsigned i = 0; i < 2; ++i) {
            if (finished[i])
                continue;
            const pid_t result = ::waitpid(children[i], &statuses[i], WNOHANG);
            if (result == children[i])
                finished[i] = true;
            else if (result < 0 && errno != EINTR) {
                relaySucceeded = false;
                finished[i] = true;
            }
        }
        const bool failedHost = finished[0] &&
            (!WIFEXITED(statuses[0]) || WEXITSTATUS(statuses[0]) != 0);
        const bool failedClient = finished[1] &&
            (!WIFEXITED(statuses[1]) || WEXITSTATUS(statuses[1]) != 0);
        if (!relaySucceeded || failedHost || failedClient)
            break;
    }

    for (unsigned i = 0; i < 2; ++i) {
        if (!finished[i]) {
            ::kill(children[i], SIGTERM);
            ::waitpid(children[i], &statuses[i], 0);
            finished[i] = true;
        }
        ::close(sockets[i][0]);
    }
    const bool bothSucceeded = finished[0] && finished[1] &&
        WIFEXITED(statuses[0]) && WEXITSTATUS(statuses[0]) == 0 &&
        WIFEXITED(statuses[1]) && WEXITSTATUS(statuses[1]) == 0;
    const auto relayElapsed = std::chrono::steady_clock::now() - relayStart;
    if (!relaySucceeded || !bothSucceeded || relayedPackets != 16 ||
        relayElapsed < std::chrono::milliseconds(500)) {
        std::cerr << "Netpacket relay diagnostic: relay=" << relaySucceeded
                  << " host_finished=" << finished[0]
                  << " host_status=" << statuses[0]
                  << " client_finished=" << finished[1]
                  << " client_status=" << statuses[1]
                  << " packets=" << relayedPackets
                  << " elapsed_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         relayElapsed).count()
                  << '\n';
    }
    expect(relaySucceeded && bothSucceeded && relayedPackets == 16 &&
               relayElapsed >= std::chrono::milliseconds(500),
           "separate core processes exchange a byte through a jittered 75-125 ms RTT relay");
}

int main(int argc, char **argv) {
    retro_set_environment(environmentCallback);
    retro_set_video_refresh(videoRefreshCallback);
    retro_set_audio_sample_batch(audioSampleBatchCallback);
    retro_set_input_poll(inputPollCallback);
    retro_set_input_state(inputStateCallback);
    retro_init();

    if (argc != 1) {
        expect(argc == 4 && (std::string(argv[1]) == "--tobu-input" || std::string(argv[1]) == "--tobudx-check"),
               "usage: libretro_core_test [--tobu-input|--tobudx-check ROM OUTPUT_DIR]");
        try {
            measureTobuInput(argv[2], argv[3], std::string(argv[1]) == "--tobudx-check");
        } catch (const std::exception &error) {
            std::cerr << "Tobu input measurement failed: " << error.what() << '\n';
            retro_unload_game();
            retro_deinit();
            return EXIT_FAILURE;
        }
        retro_unload_game();
        retro_deinit();
        return EXIT_SUCCESS;
    }

    testLibretroReportsApiAndVideoTiming();
    testLibretroLoadsOptionalBootRomFromSystemDirectory();
    testLibretroLoadsAcid2AndPublishesReferenceFrame();
    testLibretroLoadsMaximumMbc5RomSize();
    testLibretroInputChangesPongGameplayFrame();
    testLibretroPongInputResponse();
    testLibretroPublishesPulseChannelAudio();
    testLibretroLcdRestartKeepsProgressAndCoherentCallbacks();
    testLibretroExposesMbc1BatteryRam();
    testLibretroExecutesMbc1BankAndRestoresBatteryRam();
    testLibretroExposesMbc2BatteryRam();
    testMbc7LibretroSensorAndBatteryContract();
    testLibretroPersistsMbc5BatteryRamAcrossFrontendReload();
    testLibretroMbc5RumbleAndSnapshotRestore();
    testLibretroPersistsMbc3BatteryRamAcrossFrontendReload();
    testLibretroExposesAndPreservesMbc3RtcMemory();
    testLibretroReportsLoadAndStoppedFrameDiagnostics();
    testLibretroIllegalOpcodeLocksWithoutFaultingFrontend();
    testLibretroCallbackExceptionBoundaryFaultsOnce();
    testLibretroPublishesCoherentFramesAcrossLcdStartupPhases();
    testLibretroInputPulseAcrossLcdPhases();
    testLibretroSaveStateRoundTrip();
    testLibretroRepeatedRewindReplay();
    testLibretroCgbColorInputStateAndReset();
    testLibretroCgbBootRenderingStateAndReset();
    testLibretroCgbMbc1AndMbc5BatteryGameProgressReload();
    testLibretroCgbMbc3RtcGameplayAndBatteryReload();
    testLibretroCgbRtcOfflineProgression();
    testLibretroMbc30BatteryAndState();
    testLibretroCgbMbc5HighBankGameplay();
    testLibretroCgbCompatibilitySelection();
    testLibretroMbc5RumbleWithoutFrontendSupport();
    testLibretroLinkSubsystem();
    testLibretroNetpacketSerialExchange();
    testLibretroNetpacketDisconnectFaultsCore();
    testLibretroNetpacketTimeoutFaultsCore();
    testLibretroNetpacketTwoProcessExchange();

    retro_unload_game();
    retro_deinit();
    std::cout << "Libretro adapter tests passed\n";
}
