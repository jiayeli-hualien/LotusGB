#include "libretro.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <exception>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "game_boy_system.hpp"
#include "callback_exception_boundary.hpp"

namespace {

using LOTUSGB::GameBoySystem;
using LOTUSGB::JoypadButton;

constexpr unsigned VIDEO_WIDTH = LOTUSGB::DmgPpu::FRAME_WIDTH;
constexpr unsigned VIDEO_HEIGHT = LOTUSGB::DmgPpu::FRAME_HEIGHT;
constexpr size_t VIDEO_PIXEL_COUNT = VIDEO_WIDTH * VIDEO_HEIGHT;
constexpr double DMG_FRAME_RATE = 4194304.0 / GameBoySystem::DMG_FRAME_T_CYCLES;

retro_environment_t environmentCallback = nullptr;
retro_log_printf_t logCallback = nullptr;
bool frameProblemReported = false;
bool emulationFaulted = false;

void logMessage(retro_log_level level, const char *message) {
    if (logCallback)
        logCallback(level, "[LotusGB] %s\n", message);
    else
        std::fprintf(stderr, "[LotusGB] %s\n", message);
}

std::vector<uint8_t> loadOptionalDmgBootRom() {
    if (!environmentCallback)
        return {};

    const char *systemDirectory = nullptr;
    if (!environmentCallback(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY,
                             &systemDirectory) ||
        !systemDirectory || systemDirectory[0] == '\0') {
        logMessage(RETRO_LOG_INFO,
                   "No RetroArch system directory; using fast post-boot startup");
        return {};
    }

    const std::filesystem::path directory(systemDirectory);
    const char *const candidates[] = {"dmg_boot.bin", "gb_bios.bin"};
    for (const char *filename : candidates) {
        const auto path = directory / filename;
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            std::error_code error;
            const bool exists = std::filesystem::exists(path, error);
            if (error)
                throw std::runtime_error("Could not inspect DMG boot ROM path: " +
                                         path.string() + ": " + error.message());
            if (exists)
                throw std::runtime_error("Could not read DMG boot ROM: " +
                                         path.string());
            continue;
        }

        std::vector<uint8_t> bootRom(std::istreambuf_iterator<char>(file), {});
        if (file.bad())
            throw std::runtime_error("Error while reading DMG boot ROM: " +
                                     path.string());
        if (bootRom.size() != 0x100)
            throw std::runtime_error("DMG boot ROM must be exactly 256 bytes: " +
                                     path.string());

        const std::string message = "Using DMG boot ROM: " + path.string();
        logMessage(RETRO_LOG_INFO, message.c_str());
        return bootRom;
    }

    logMessage(RETRO_LOG_INFO,
               "No DMG boot ROM found in the system directory; using fast post-boot startup");
    return {};
}

retro_video_refresh_t videoRefreshCallback = nullptr;
retro_audio_sample_t audioSampleCallback = nullptr;
retro_audio_sample_batch_t audioSampleBatchCallback = nullptr;
retro_input_poll_t inputPollCallback = nullptr;
retro_input_state_t inputStateCallback = nullptr;

std::vector<uint8_t> loadedRom;
std::vector<uint8_t> loadedDmgBootRom;
std::unique_ptr<GameBoySystem> emulatorSystem;
bool useXrgb8888 = false;
std::array<uint32_t, VIDEO_PIXEL_COUNT> xrgb8888Frame{};
std::array<uint16_t, VIDEO_PIXEL_COUNT> rgb1555Frame{};

class AudioDiagnostics {
public:
    void capture(const std::vector<int16_t> &samples, size_t submittedFrames,
                 size_t processedFrames) {
        initializeIfNeeded();
        if (outputPath.empty() || pcmSamples.size() >= MAX_CAPTURE_SAMPLES)
            return;

        const size_t remainingSamples = MAX_CAPTURE_SAMPLES - pcmSamples.size();
        const size_t capturedSamples = std::min(remainingSamples, samples.size());
        pcmSamples.insert(pcmSamples.end(), samples.begin(),
                          samples.begin() + capturedSamples);
        batches.emplace_back(submittedFrames, processedFrames,
                             capturedSamples / 2);
    }

    void flush() {
        if (outputPath.empty() || pcmSamples.empty()) {
            reset();
            return;
        }

        std::ofstream pcmFile(outputPath, std::ios::binary | std::ios::trunc);
        for (int16_t sample : pcmSamples) {
            const uint16_t bits = static_cast<uint16_t>(sample);
            pcmFile.put(static_cast<char>(bits & 0xff));
            pcmFile.put(static_cast<char>((bits >> 8) & 0xff));
        }

        std::ofstream batchFile(outputPath + ".frames.csv",
                                std::ios::trunc);
        batchFile << "submitted_frames,processed_frames,captured_frames\n";
        for (const auto &[submitted, processed, captured] : batches)
            batchFile << submitted << ',' << processed << ',' << captured << '\n';

        reset();
    }

private:
    static constexpr size_t MAX_CAPTURE_SAMPLES = 48000 * 2 * 30;

    void initializeIfNeeded() {
        if (initialized)
            return;
        initialized = true;
        const char *configuredPath = std::getenv("LOTUSGB_AUDIO_DUMP");
        if (configuredPath)
            outputPath = configuredPath;
    }

    void reset() {
        initialized = false;
        outputPath.clear();
        pcmSamples.clear();
        batches.clear();
    }

    bool initialized = false;
    std::string outputPath;
    std::vector<int16_t> pcmSamples;
    std::vector<std::tuple<size_t, size_t, size_t>> batches;
};

AudioDiagnostics audioDiagnostics;

struct ButtonBinding {
    unsigned retroButton;
    JoypadButton gbButton;
};

constexpr ButtonBinding BUTTON_BINDINGS[] = {
    {RETRO_DEVICE_ID_JOYPAD_UP, JoypadButton::Up},
    {RETRO_DEVICE_ID_JOYPAD_DOWN, JoypadButton::Down},
    {RETRO_DEVICE_ID_JOYPAD_LEFT, JoypadButton::Left},
    {RETRO_DEVICE_ID_JOYPAD_RIGHT, JoypadButton::Right},
    {RETRO_DEVICE_ID_JOYPAD_A, JoypadButton::A},
    {RETRO_DEVICE_ID_JOYPAD_B, JoypadButton::B},
    {RETRO_DEVICE_ID_JOYPAD_SELECT, JoypadButton::Select},
    {RETRO_DEVICE_ID_JOYPAD_START, JoypadButton::Start},
};

retro_input_descriptor INPUT_DESCRIPTORS[] = {
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Game Boy A"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Game Boy B"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
    {0, 0, 0, 0, nullptr},
};

bool isSupportedRom(const std::vector<uint8_t> &rom) {
    constexpr size_t MINIMUM_HEADER_SIZE = 0x150;
    constexpr size_t MAX_BANKED_ROM_SIZE = 0x200000;
    constexpr size_t CARTRIDGE_TYPE_OFFSET = 0x0147;
    if (rom.size() < MINIMUM_HEADER_SIZE)
        return false;

    const uint8_t type = rom[CARTRIDGE_TYPE_OFFSET];
    if (type == 0x00)
        return rom.size() <= 0x8000;
    if (type == 0x05 || type == 0x06)
        return rom.size() >= 0x8000 && rom.size() <= 0x40000 &&
               rom.size() % 0x4000 == 0;
    if (type >= 0x19 && type <= 0x1E)
        return rom.size() >= 0x8000 && rom.size() <= 0x800000 &&
               rom.size() % 0x4000 == 0;
    if (type >= 0x0F && type <= 0x13)
        return rom.size() >= 0x8000 && rom.size() <= 0x400000 &&
               rom.size() % 0x4000 == 0;
    const bool supportedBankedType =
        type == 0x01 || type == 0x02 || type == 0x03;
    return supportedBankedType && rom.size() >= 0x8000 &&
           rom.size() <= MAX_BANKED_ROM_SIZE &&
           rom.size() % 0x4000 == 0;
}

void pollInput() {
    if (inputPollCallback)
        inputPollCallback();

    if (!emulatorSystem)
        return;

    for (const ButtonBinding &binding : BUTTON_BINDINGS) {
        const bool pressed = inputStateCallback &&
            inputStateCallback(0, RETRO_DEVICE_JOYPAD, 0, binding.retroButton) != 0;
        emulatorSystem->setButtonState(binding.gbButton, pressed);
    }
}

void renderFrame() {
    if (!emulatorSystem || !videoRefreshCallback)
        return;

    const auto &shades = emulatorSystem->getCompletedFrameBuffer();
    const bool color = emulatorSystem->getHardwareModel() == LOTUSGB::HardwareModel::CgbE;
    const auto &colors = emulatorSystem->getCompletedColorFrameBuffer();
    for (size_t i = 0; i < shades.size(); ++i) {
        const uint8_t shade = std::min<uint8_t>(shades[i], 3);
        const uint8_t gray = static_cast<uint8_t>(255 - shade * 85);
        const uint16_t rgb = colors[i];
        const auto expand = [](unsigned x) { return (x << 3) | (x >> 2); };
        const uint8_t red = color ? expand(rgb & 31) : gray;
        const uint8_t green = color ? expand((rgb >> 5) & 31) : gray;
        const uint8_t blue = color ? expand((rgb >> 10) & 31) : gray;
        if (useXrgb8888) {
            xrgb8888Frame[i] = (static_cast<uint32_t>(red) << 16) |
                               (static_cast<uint32_t>(green) << 8) | blue;
        } else {
            rgb1555Frame[i] = static_cast<uint16_t>(((red >> 3) << 10) |
                                                    ((green >> 3) << 5) | (blue >> 3));
        }
    }

    if (useXrgb8888) {
        videoRefreshCallback(xrgb8888Frame.data(), VIDEO_WIDTH, VIDEO_HEIGHT,
                             VIDEO_WIDTH * sizeof(uint32_t));
    } else {
        videoRefreshCallback(rgb1555Frame.data(), VIDEO_WIDTH, VIDEO_HEIGHT,
                             VIDEO_WIDTH * sizeof(uint16_t));
    }
}

void submitAudio() {
    if (!emulatorSystem)
        return;

    const std::vector<int16_t> samples = emulatorSystem->takeAudioSamples();
    const size_t frameCount = samples.size() / 2;
    if (frameCount == 0)
        return;

    if (audioSampleBatchCallback) {
        const size_t processedFrames =
            audioSampleBatchCallback(samples.data(), frameCount);
        audioDiagnostics.capture(samples, frameCount, processedFrames);
    } else if (audioSampleCallback) {
        for (size_t frame = 0; frame < frameCount; ++frame)
            audioSampleCallback(samples[frame * 2], samples[frame * 2 + 1]);
        audioDiagnostics.capture(samples, frameCount, frameCount);
    }
}

} // namespace

extern "C" {

void retro_set_environment(retro_environment_t callback) {
    environmentCallback = callback;
}

void retro_set_video_refresh(retro_video_refresh_t callback) {
    videoRefreshCallback = callback;
}

void retro_set_audio_sample(retro_audio_sample_t callback) {
    audioSampleCallback = callback;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) {
    audioSampleBatchCallback = callback;
}

void retro_set_input_poll(retro_input_poll_t callback) {
    inputPollCallback = callback;
}

void retro_set_input_state(retro_input_state_t callback) {
    inputStateCallback = callback;
}

unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

void retro_init(void) {
    useXrgb8888 = false;
    logCallback = nullptr;
    frameProblemReported = false;
    emulationFaulted = false;
    if (environmentCallback) {
        retro_log_callback logging{};
        if (environmentCallback(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
            logCallback = logging.log;
        unsigned pixelFormat = RETRO_PIXEL_FORMAT_XRGB8888;
        useXrgb8888 = environmentCallback(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT,
                                          &pixelFormat);
        environmentCallback(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
                            INPUT_DESCRIPTORS);
    }
}

void retro_deinit(void) {
    logCallback = nullptr;
    audioDiagnostics.flush();
    emulatorSystem.reset();
    loadedRom.clear();
    loadedDmgBootRom.clear();
    frameProblemReported = false;
    emulationFaulted = false;
}

void retro_get_system_info(retro_system_info *info) {
    if (!info)
        return;
    info->library_name = "LotusGB";
    info->library_version = "0.1";
    info->valid_extensions = "gb|gbc";
    info->need_fullpath = false;
    info->block_extract = false;
}

void retro_get_system_av_info(retro_system_av_info *info) {
    if (!info)
        return;
    info->geometry.base_width = VIDEO_WIDTH;
    info->geometry.base_height = VIDEO_HEIGHT;
    info->geometry.max_width = VIDEO_WIDTH;
    info->geometry.max_height = VIDEO_HEIGHT;
    info->geometry.aspect_ratio = 10.0f / 9.0f;
    info->timing.fps = DMG_FRAME_RATE;
    info->timing.sample_rate = 48000.0;
}

void retro_set_controller_port_device(unsigned, unsigned) {}

void retro_reset(void) {
    if (loadedRom.empty())
        return;
    try {
        const auto saveRam = emulatorSystem
                                 ? emulatorSystem->getSaveRamStorage()
                                 : nullptr;
        const auto saveRtc = emulatorSystem
                                 ? emulatorSystem->getRtcStorage()
                                 : nullptr;
        auto newSystem =
            std::make_unique<GameBoySystem>(loadedRom, (loadedRom[0x143] & 0x80)
                ? LOTUSGB::HardwareModel::CgbE : LOTUSGB::HardwareModel::Dmg,
                saveRam, saveRtc, loadedDmgBootRom);
        emulatorSystem = std::move(newSystem);
        frameProblemReported = false;
        emulationFaulted = false;
    } catch (const std::exception &error) {
        logMessage(RETRO_LOG_ERROR, error.what());
        emulatorSystem.reset();
    } catch (...) {
        logMessage(RETRO_LOG_ERROR, "Unexpected error resetting the core");
        emulatorSystem.reset();
    }
}

void retro_run(void) {
    if (!emulatorSystem)
        return;

    LOTUSGB::LibretroInternal::invokeCallbackSafely(
        emulationFaulted,
        [] {
            pollInput();
#ifdef LOTUSGB_TEST_FIXED_FRAME_STEP
            // Fixed-duration diagnostic baseline; not the distributed default.
            const auto step = emulatorSystem->stepFrame();
            const bool success = step.success;
            const bool stopped = step.success && !step.frameCompleted;
#else
            const auto step = emulatorSystem->stepForPresentation();
            const bool success = step.reason != LOTUSGB::PresentationReason::CpuFailure;
            const bool stopped = step.reason == LOTUSGB::PresentationReason::CpuStopped;
#endif
            // A bounded Repeat is expected while LCD is off/restarting. It
            // still submits one frame and the PCM generated by actual clocks.
            if (!success || stopped) {
                if (!frameProblemReported) {
                    char message[192];
                    auto registers = emulatorSystem->getCpuRegisters();
                    std::snprintf(message, sizeof(message),
                                  "%s; PC=0x%04X frame_tcycles=%llu",
                                  success
                                      ? "CPU clock stopped before frame completion (STOP)"
                                      : "CPU execution failed before frame completion",
                                  static_cast<unsigned>(registers.getRefPC()),
                                  static_cast<unsigned long long>(step.tCyclesElapsed));
                    logMessage(success ? RETRO_LOG_WARN : RETRO_LOG_ERROR,
                               message);
                }
                frameProblemReported = true;
                if (!success)
                    emulationFaulted = true;
            } else {
                frameProblemReported = false;
            }
            renderFrame();
            submitAudio();
        },
        [](const char *error) {
            char message[256];
            std::snprintf(message, sizeof(message),
                          "Exception during retro_run: %s", error);
            logMessage(RETRO_LOG_ERROR, message);
        });
}

size_t retro_serialize_size(void) {
    return emulatorSystem ? GameBoySystem::MAX_STATE_SIZE : 0;
}

bool retro_serialize(void *data, size_t size) {
    if (!emulatorSystem || emulationFaulted || !data || size < GameBoySystem::MAX_STATE_SIZE)
        return false;
    // Snapshot failures are recoverable: the running system is not mutated.
    bool snapshotFailed = false;
    return LOTUSGB::LibretroInternal::invokeCallbackSafely(
        snapshotFailed, [&] {
            const auto state = emulatorSystem->saveState();
            std::memset(data, 0, size);
            std::memcpy(data, state.data(), state.size());
        }, [](const char *error) { logMessage(RETRO_LOG_ERROR, error); });
}

bool retro_unserialize(const void *data, size_t size) {
    if (!emulatorSystem || !data)
        return false;
    bool snapshotFailed = false;
    return LOTUSGB::LibretroInternal::invokeCallbackSafely(
        snapshotFailed, [&] {
            emulatorSystem->loadState(data, size);
            // A valid snapshot may recover a machine faulted after the save.
            emulationFaulted = false;
            frameProblemReported = false;
        }, [](const char *error) { logMessage(RETRO_LOG_ERROR, error); });
}

void retro_cheat_reset(void) {}

void retro_cheat_set(unsigned, bool, const char *) {}

bool retro_load_game(const retro_game_info *game) {
    constexpr size_t MINIMUM_ROM_SIZE = 0x150;
    constexpr size_t MAXIMUM_ROM_SIZE = 0x800000;
    if (!game || !game->data || game->size < MINIMUM_ROM_SIZE ||
        game->size > MAXIMUM_ROM_SIZE) {
        logMessage(RETRO_LOG_ERROR, "Missing ROM content or ROM size outside 336 bytes to 8 MiB");
        return false;
    }

    const auto *data = static_cast<const uint8_t *>(game->data);
    try {
        std::vector<uint8_t> rom(data, data + game->size);
        if (!isSupportedRom(rom)) {
            char message[128];
            std::snprintf(message, sizeof(message),
                          "Unsupported cartridge type/ROM size: type=0x%02X bytes=%zu",
                          static_cast<unsigned>(rom[0x147]), rom.size());
            logMessage(RETRO_LOG_ERROR, message);
            return false;
        }
        const auto model = (rom[0x143] & 0x80) ? LOTUSGB::HardwareModel::CgbE : LOTUSGB::HardwareModel::Dmg;
        std::vector<uint8_t> bootRom = model == LOTUSGB::HardwareModel::Dmg ? loadOptionalDmgBootRom() : std::vector<uint8_t>{};
        if (model == LOTUSGB::HardwareModel::CgbE)
            logMessage(RETRO_LOG_INFO, "Native CGB: fast post-boot startup");
        std::unique_ptr<GameBoySystem> newSystem =
            std::make_unique<GameBoySystem>(rom, model, nullptr, nullptr, bootRom);
        loadedRom = std::move(rom);
        loadedDmgBootRom = std::move(bootRom);
        emulatorSystem = std::move(newSystem);
        frameProblemReported = false;
        emulationFaulted = false;
    } catch (const std::exception &error) {
        logMessage(RETRO_LOG_ERROR, error.what());
        return false;
    } catch (...) {
        logMessage(RETRO_LOG_ERROR, "Unexpected error loading cartridge");
        return false;
    }
    return true;
}

bool retro_load_game_special(unsigned, const retro_game_info *, size_t) {
    return false;
}

void retro_unload_game(void) {
    audioDiagnostics.flush();
    emulatorSystem.reset();
    loadedRom.clear();
    loadedDmgBootRom.clear();
    frameProblemReported = false;
    emulationFaulted = false;
}

unsigned retro_get_region(void) {
    return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id) {
    if (!emulatorSystem)
        return nullptr;
    if (id == RETRO_MEMORY_SAVE_RAM)
        return emulatorSystem->getSaveRamData();
    if (id == RETRO_MEMORY_RTC)
        return emulatorSystem->getRtcData();
    return nullptr;
}

size_t retro_get_memory_size(unsigned id) {
    if (!emulatorSystem)
        return 0;
    if (id == RETRO_MEMORY_SAVE_RAM)
        return emulatorSystem->getSaveRamSize();
    if (id == RETRO_MEMORY_RTC)
        return emulatorSystem->getRtcSize();
    return 0;
}

} // extern "C"
