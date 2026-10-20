#include "libretro.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "game_boy_system.hpp"

namespace {

using LOTUSGB::GameBoySystem;
using LOTUSGB::JoypadButton;

constexpr unsigned VIDEO_WIDTH = LOTUSGB::DmgPpu::FRAME_WIDTH;
constexpr unsigned VIDEO_HEIGHT = LOTUSGB::DmgPpu::FRAME_HEIGHT;
constexpr size_t VIDEO_PIXEL_COUNT = VIDEO_WIDTH * VIDEO_HEIGHT;
constexpr double DMG_FRAME_RATE = 4194304.0 / GameBoySystem::DMG_FRAME_T_CYCLES;

retro_environment_t environmentCallback = nullptr;
retro_video_refresh_t videoRefreshCallback = nullptr;
retro_audio_sample_t audioSampleCallback = nullptr;
retro_audio_sample_batch_t audioSampleBatchCallback = nullptr;
retro_input_poll_t inputPollCallback = nullptr;
retro_input_state_t inputStateCallback = nullptr;

std::vector<uint8_t> loadedRom;
std::unique_ptr<GameBoySystem> emulatorSystem;
bool useXrgb8888 = false;
std::array<uint32_t, VIDEO_PIXEL_COUNT> xrgb8888Frame{};
std::array<uint16_t, VIDEO_PIXEL_COUNT> rgb1555Frame{};

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
    constexpr size_t MAX_ROM_ONLY_SIZE = 0x8000;
    constexpr size_t CARTRIDGE_TYPE_OFFSET = 0x0147;
    return rom.size() >= MINIMUM_HEADER_SIZE &&
           rom.size() <= MAX_ROM_ONLY_SIZE &&
           rom[CARTRIDGE_TYPE_OFFSET] == 0x00;
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

    const auto &shades = emulatorSystem->getFrameBuffer();
    for (size_t i = 0; i < shades.size(); ++i) {
        const uint8_t shade = std::min<uint8_t>(shades[i], 3);
        const uint8_t gray = static_cast<uint8_t>(255 - shade * 85);
        if (useXrgb8888) {
            xrgb8888Frame[i] = (static_cast<uint32_t>(gray) << 16) |
                               (static_cast<uint32_t>(gray) << 8) | gray;
        } else {
            const uint16_t channel = gray >> 3;
            rgb1555Frame[i] = static_cast<uint16_t>((channel << 10) |
                                                    (channel << 5) | channel);
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
        audioSampleBatchCallback(samples.data(), frameCount);
    } else if (audioSampleCallback) {
        for (size_t frame = 0; frame < frameCount; ++frame)
            audioSampleCallback(samples[frame * 2], samples[frame * 2 + 1]);
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
    if (environmentCallback) {
        unsigned pixelFormat = RETRO_PIXEL_FORMAT_XRGB8888;
        useXrgb8888 = environmentCallback(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT,
                                          &pixelFormat);
        environmentCallback(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
                            INPUT_DESCRIPTORS);
    }
}

void retro_deinit(void) {
    emulatorSystem.reset();
    loadedRom.clear();
}

void retro_get_system_info(retro_system_info *info) {
    if (!info)
        return;
    info->library_name = "LotusGB";
    info->library_version = "0.1";
    info->valid_extensions = "gb";
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
        emulatorSystem = std::make_unique<GameBoySystem>(loadedRom);
    } catch (...) {
        emulatorSystem.reset();
    }
}

void retro_run(void) {
    if (!emulatorSystem)
        return;

    pollInput();
    emulatorSystem->stepFrame();
    renderFrame();
    submitAudio();
}

size_t retro_serialize_size(void) {
    return 0;
}

bool retro_serialize(void *, size_t) {
    return false;
}

bool retro_unserialize(const void *, size_t) {
    return false;
}

void retro_cheat_reset(void) {}

void retro_cheat_set(unsigned, bool, const char *) {}

bool retro_load_game(const retro_game_info *game) {
    constexpr size_t MINIMUM_ROM_SIZE = 0x150;
    constexpr size_t MAXIMUM_ROM_SIZE = 0x8000;
    constexpr size_t CARTRIDGE_TYPE_OFFSET = 0x0147;
    if (!game || !game->data || game->size < MINIMUM_ROM_SIZE ||
        game->size > MAXIMUM_ROM_SIZE)
        return false;

    const auto *data = static_cast<const uint8_t *>(game->data);
    if (data[CARTRIDGE_TYPE_OFFSET] != 0x00)
        return false;

    try {
        std::vector<uint8_t> rom(data, data + game->size);
        if (!isSupportedRom(rom))
            return false;
        std::unique_ptr<GameBoySystem> newSystem =
            std::make_unique<GameBoySystem>(rom);
        loadedRom = std::move(rom);
        emulatorSystem = std::move(newSystem);
    } catch (...) {
        return false;
    }
    return true;
}

bool retro_load_game_special(unsigned, const retro_game_info *, size_t) {
    return false;
}

void retro_unload_game(void) {
    emulatorSystem.reset();
    loadedRom.clear();
}

unsigned retro_get_region(void) {
    return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned) {
    return nullptr;
}

size_t retro_get_memory_size(unsigned) {
    return 0;
}

} // extern "C"
