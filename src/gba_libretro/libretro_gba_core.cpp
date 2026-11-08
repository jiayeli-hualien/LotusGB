#include "libretro.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "gba_system.hpp"

namespace {

using LOTUSGBA::GbaSystem;

constexpr uint64_t GBA_CLOCK_HZ = 16777216;
constexpr double GBA_FPS = static_cast<double>(GBA_CLOCK_HZ) /
                           GbaSystem::CYCLES_PER_FRAME;
constexpr unsigned WIDTH = GbaSystem::SCREEN_WIDTH;
constexpr unsigned HEIGHT = GbaSystem::SCREEN_HEIGHT;
constexpr size_t PIXELS = GbaSystem::PIXEL_COUNT;

retro_environment_t environmentCallback = nullptr;
retro_log_printf_t logCallback = nullptr;
retro_video_refresh_t videoCallback = nullptr;
retro_audio_sample_t audioCallback = nullptr;
retro_audio_sample_batch_t audioBatchCallback = nullptr;
retro_input_poll_t inputPollCallback = nullptr;
retro_input_state_t inputStateCallback = nullptr;

std::unique_ptr<GbaSystem> gbaSystem;
std::vector<uint8_t> loadedRom;
std::array<uint32_t, PIXELS> xrgbFrame{};
std::array<uint16_t, PIXELS> rgb1555Frame{};
bool useXrgb8888 = false;
bool coreFaulted = false;

enum class BiosStartupMode {
    Automatic,
    RequireExternal,
    SkipExternal,
};

constexpr const char *BIOS_MODE_OPTION = "lotusgba_bios_mode";

void registerCoreOptions() {
    if (!environmentCallback)
        return;
    retro_variable options[] = {
        {BIOS_MODE_OPTION,
         "GBA BIOS startup (restart content to apply); Automatic|Use BIOS|Skip BIOS"},
        {nullptr, nullptr},
    };
    environmentCallback(RETRO_ENVIRONMENT_SET_VARIABLES, options);
}

void logMessage(retro_log_level level, const char *message) {
    if (logCallback)
        logCallback(level, "[LotusGBA] %s\n", message);
    else
        std::fprintf(stderr, "[LotusGBA] %s\n", message);
}

BiosStartupMode readBiosStartupMode() {
    if (!environmentCallback)
        return BiosStartupMode::Automatic;

    retro_variable option{BIOS_MODE_OPTION, nullptr};
    if (!environmentCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &option) ||
        !option.value || std::string(option.value) == "Automatic")
        return BiosStartupMode::Automatic;
    if (std::string(option.value) == "Use BIOS")
        return BiosStartupMode::RequireExternal;
    if (std::string(option.value) == "Skip BIOS")
        return BiosStartupMode::SkipExternal;

    logMessage(RETRO_LOG_WARN,
               "Unknown BIOS startup option; using Automatic mode");
    return BiosStartupMode::Automatic;
}

std::vector<uint8_t> loadBios(BiosStartupMode mode) {
    if (mode == BiosStartupMode::SkipExternal) {
        logMessage(RETRO_LOG_INFO,
                   "Skipping external BIOS; using initialized HLE cartridge startup");
        return {};
    }

    if (!environmentCallback) {
        if (mode == BiosStartupMode::RequireExternal)
            throw std::runtime_error(
                "GBA BIOS startup is set to Use BIOS, but RetroArch has no system directory");
        logMessage(RETRO_LOG_INFO,
                   "No RetroArch system directory; using HLE BIOS startup");
        return {};
    }

    const char *systemDirectory = nullptr;
    if (!environmentCallback(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY,
                             &systemDirectory) ||
        !systemDirectory || systemDirectory[0] == '\0') {
        if (mode == BiosStartupMode::RequireExternal)
            throw std::runtime_error(
                "GBA BIOS startup is set to Use BIOS, but RetroArch has no system directory");
        logMessage(RETRO_LOG_INFO,
                   "No RetroArch system directory; using HLE BIOS startup");
        return {};
    }

    const std::filesystem::path directory(systemDirectory);
    constexpr const char *filenames[] = {"gba_bios.bin", "bios.bin"};
    for (const char *filename : filenames) {
        const auto path = directory / filename;
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            std::error_code error;
            const bool exists = std::filesystem::exists(path, error);
            if (error)
                throw std::runtime_error("Could not inspect GBA BIOS path: " +
                                         path.string() + ": " + error.message());
            if (exists)
                throw std::runtime_error("Could not read GBA BIOS: " +
                                         path.string());
            continue;
        }

        std::vector<uint8_t> bios(std::istreambuf_iterator<char>(file), {});
        if (file.bad())
            throw std::runtime_error("Error while reading GBA BIOS: " +
                                     path.string());
        if (bios.size() != 0x4000)
            throw std::runtime_error("GBA BIOS must be exactly 16 KiB: " +
                                     path.string());

        const std::string message = "Using GBA BIOS: " + path.string();
        logMessage(RETRO_LOG_INFO, message.c_str());
        return bios;
    }

    if (mode == BiosStartupMode::RequireExternal)
        throw std::runtime_error(
            "GBA BIOS startup is set to Use BIOS, but gba_bios.bin or bios.bin was not found");
    logMessage(RETRO_LOG_INFO,
               "No GBA BIOS found; using HLE BIOS startup");
    return {};
}

uint32_t expand5(uint32_t value) {
    value &= 31;
    return (value << 3) | (value >> 2);
}

void submitVideo() {
    if (!videoCallback || !gbaSystem)
        return;
    const auto &source = gbaSystem->frameBuffer();
    if (useXrgb8888) {
        for (size_t i = 0; i < PIXELS; ++i) {
            const uint16_t color = source[i];
            const uint32_t red = expand5(color);
            const uint32_t green = expand5(color >> 5);
            const uint32_t blue = expand5(color >> 10);
            xrgbFrame[i] = (red << 16) | (green << 8) | blue;
        }
        videoCallback(xrgbFrame.data(), WIDTH, HEIGHT, WIDTH * sizeof(uint32_t));
        return;
    }
    for (size_t i = 0; i < PIXELS; ++i) {
        const uint16_t color = source[i];
        const uint16_t red = color & 0x1F;
        const uint16_t green = (color >> 5) & 0x1F;
        const uint16_t blue = (color >> 10) & 0x1F;
        rgb1555Frame[i] = static_cast<uint16_t>((red << 10) | (green << 5) | blue);
    }
    videoCallback(rgb1555Frame.data(), WIDTH, HEIGHT, WIDTH * sizeof(uint16_t));
}

void submitAudio() {
    if (!gbaSystem)
        return;
    const std::vector<int16_t> samples = gbaSystem->takeAudioSamples();
    const size_t frames = samples.size() / 2;
    if (audioBatchCallback) {
        audioBatchCallback(samples.data(), frames);
    } else if (audioCallback) {
        for (size_t i = 0; i < frames; ++i)
            audioCallback(samples[i * 2], samples[i * 2 + 1]);
    }
}

uint16_t readPressedButtons() {
    if (!inputStateCallback)
        return 0;
    struct Binding {
        unsigned retroId;
        uint16_t gbaMask;
    };
    constexpr Binding bindings[] = {
        {RETRO_DEVICE_ID_JOYPAD_A, 1u << 0},
        {RETRO_DEVICE_ID_JOYPAD_B, 1u << 1},
        {RETRO_DEVICE_ID_JOYPAD_SELECT, 1u << 2},
        {RETRO_DEVICE_ID_JOYPAD_START, 1u << 3},
        {RETRO_DEVICE_ID_JOYPAD_RIGHT, 1u << 4},
        {RETRO_DEVICE_ID_JOYPAD_LEFT, 1u << 5},
        {RETRO_DEVICE_ID_JOYPAD_UP, 1u << 6},
        {RETRO_DEVICE_ID_JOYPAD_DOWN, 1u << 7},
        {RETRO_DEVICE_ID_JOYPAD_R, 1u << 8},
        {RETRO_DEVICE_ID_JOYPAD_L, 1u << 9},
    };
    uint16_t pressed = 0;
    for (const Binding &binding : bindings) {
        if (inputStateCallback(0, RETRO_DEVICE_JOYPAD, 0, binding.retroId))
            pressed |= binding.gbaMask;
    }
    return pressed;
}

} // namespace

extern "C" {

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_set_environment(retro_environment_t callback) {
    environmentCallback = callback;
    if (environmentCallback) {
        bool noGame = false;
        environmentCallback(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noGame);
        registerCoreOptions();
    }
}

void retro_set_video_refresh(retro_video_refresh_t callback) { videoCallback = callback; }
void retro_set_audio_sample(retro_audio_sample_t callback) { audioCallback = callback; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) { audioBatchCallback = callback; }
void retro_set_input_poll(retro_input_poll_t callback) { inputPollCallback = callback; }
void retro_set_input_state(retro_input_state_t callback) { inputStateCallback = callback; }

void retro_init(void) {
    coreFaulted = false;
    if (environmentCallback) {
        retro_log_callback logging{};
        if (environmentCallback(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
            logCallback = logging.log;
        enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
        useXrgb8888 = environmentCallback(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);
        if (!useXrgb8888) {
            format = RETRO_PIXEL_FORMAT_0RGB1555;
            environmentCallback(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);
        }
    }
}

void retro_deinit(void) {
    gbaSystem.reset();
    loadedRom.clear();
    logCallback = nullptr;
    coreFaulted = false;
}

void retro_get_system_info(retro_system_info *info) {
    if (!info)
        return;
    info->library_name = "LotusGBA";
    info->library_version = "0.1";
    info->valid_extensions = "gba";
    info->need_fullpath = false;
    info->block_extract = false;
}

void retro_get_system_av_info(retro_system_av_info *info) {
    if (!info)
        return;
    info->geometry.base_width = WIDTH;
    info->geometry.base_height = HEIGHT;
    info->geometry.max_width = WIDTH;
    info->geometry.max_height = HEIGHT;
    info->geometry.aspect_ratio = 3.0f / 2.0f;
    info->timing.fps = GBA_FPS;
    info->timing.sample_rate = GbaSystem::AUDIO_SAMPLE_RATE;
}

void retro_set_controller_port_device(unsigned, unsigned) {}

void retro_reset(void) {
    if (!gbaSystem)
        return;
    gbaSystem->reset();
    coreFaulted = false;
}

void retro_run(void) {
    if (!gbaSystem || coreFaulted)
        return;
    try {
        if (inputPollCallback)
            inputPollCallback();
        gbaSystem->setPressedButtons(readPressedButtons());
        const auto result = gbaSystem->runFrame();
        if (!result.success) {
            logMessage(RETRO_LOG_ERROR, gbaSystem->fault().c_str());
            coreFaulted = true;
            return;
        }
        submitVideo();
        submitAudio();
    } catch (const std::exception &error) {
        logMessage(RETRO_LOG_ERROR, error.what());
        coreFaulted = true;
    } catch (...) {
        logMessage(RETRO_LOG_ERROR, "Unexpected exception during retro_run");
        coreFaulted = true;
    }
}

size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *, size_t) { return false; }
bool retro_unserialize(const void *, size_t) { return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned, bool, const char *) {}

bool retro_load_game(const retro_game_info *game) {
    if (!game || !game->data || game->size < 0xC4 || game->size > 32 * 1024 * 1024) {
        logMessage(RETRO_LOG_ERROR, "GBA content must contain at least 0xC4 bytes and be at most 32 MiB");
        return false;
    }
    try {
        const auto *begin = static_cast<const uint8_t *>(game->data);
        std::vector<uint8_t> candidate(begin, begin + game->size);
        auto candidateBios = loadBios(readBiosStartupMode());
        std::shared_ptr<std::vector<uint8_t>> candidateSaveFlash;
        if (gbaSystem && loadedRom == candidate)
            candidateSaveFlash = gbaSystem->getSaveRamStorage();
        auto candidateSystem = std::make_unique<GbaSystem>(
            candidate, std::move(candidateBios), std::move(candidateSaveFlash));
        loadedRom = std::move(candidate);
        gbaSystem = std::move(candidateSystem);
        coreFaulted = false;
    } catch (const std::exception &error) {
        logMessage(RETRO_LOG_ERROR, error.what());
        return false;
    } catch (...) {
        logMessage(RETRO_LOG_ERROR, "Unexpected error loading GBA content");
        return false;
    }
    return true;
}

bool retro_load_game_special(unsigned, const retro_game_info *, size_t) { return false; }
void retro_unload_game(void) { gbaSystem.reset(); loadedRom.clear(); coreFaulted = false; }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned id) {
    if (!gbaSystem || id != RETRO_MEMORY_SAVE_RAM)
        return nullptr;
    return gbaSystem->getSaveRamData();
}

size_t retro_get_memory_size(unsigned id) {
    if (!gbaSystem || id != RETRO_MEMORY_SAVE_RAM)
        return 0;
    return gbaSystem->getSaveRamSize();
}

} // extern "C"
