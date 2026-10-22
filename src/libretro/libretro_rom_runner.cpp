#include "libretro.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr unsigned WIDTH = 160;
constexpr unsigned HEIGHT = 144;
constexpr size_t PIXEL_COUNT = WIDTH * HEIGHT;

std::array<uint32_t, PIXEL_COUNT> lastFrame{};
unsigned videoFrameCount = 0;
size_t audioFrameCount = 0;
bool hasVideoFrame = false;
std::string callbackError;

bool environmentCallback(unsigned command, void *data) {
    if (command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) {
        return data && *static_cast<unsigned *>(data) ==
                           RETRO_PIXEL_FORMAT_XRGB8888;
    }
    if (command == RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS)
        return true;
    return false;
}

void videoRefreshCallback(const void *data, unsigned width, unsigned height,
                          size_t pitch) {
    ++videoFrameCount;
    if (!data || width != WIDTH || height != HEIGHT ||
        pitch < WIDTH * sizeof(uint32_t)) {
        callbackError = "core returned an invalid video frame";
        return;
    }

    const auto *bytes = static_cast<const uint8_t *>(data);
    for (unsigned row = 0; row < HEIGHT; ++row) {
        std::memcpy(lastFrame.data() + row * WIDTH, bytes + row * pitch,
                    WIDTH * sizeof(uint32_t));
    }
    hasVideoFrame = true;
}

size_t audioSampleBatchCallback(const int16_t *, size_t frames) {
    audioFrameCount += frames;
    return frames;
}

void inputPollCallback() {}

int16_t inputStateCallback(unsigned, unsigned, unsigned, unsigned) {
    return 0;
}

template <typename Function>
Function loadFunction(void *library, const char *name) {
    void *symbol = dlsym(library, name);
    if (!symbol)
        throw std::runtime_error(std::string("missing core symbol: ") + name);

    Function function{};
    static_assert(sizeof(function) == sizeof(symbol),
                  "function and data pointers must have matching sizes");
    std::memcpy(&function, &symbol, sizeof(function));
    return function;
}

struct CoreApi {
    using SetEnvironment = void (*)(retro_environment_t);
    using SetVideoRefresh = void (*)(retro_video_refresh_t);
    using SetAudioSample = void (*)(retro_audio_sample_t);
    using SetAudioSampleBatch = void (*)(retro_audio_sample_batch_t);
    using SetInputPoll = void (*)(retro_input_poll_t);
    using SetInputState = void (*)(retro_input_state_t);
    using Init = void (*)(void);
    using Deinit = void (*)(void);
    using LoadGame = bool (*)(const retro_game_info *);
    using Run = void (*)(void);
    using UnloadGame = void (*)(void);
    using GetMemoryData = void *(*)(unsigned);
    using GetMemorySize = size_t (*)(unsigned);

    explicit CoreApi(const char *path) {
        library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!library)
            throw std::runtime_error(std::string("cannot load core: ") +
                                     dlerror());

        setEnvironment = loadFunction<SetEnvironment>(library, "retro_set_environment");
        setVideoRefresh = loadFunction<SetVideoRefresh>(library, "retro_set_video_refresh");
        setAudioSample = loadFunction<SetAudioSample>(library, "retro_set_audio_sample");
        setAudioSampleBatch =
            loadFunction<SetAudioSampleBatch>(library, "retro_set_audio_sample_batch");
        setInputPoll = loadFunction<SetInputPoll>(library, "retro_set_input_poll");
        setInputState = loadFunction<SetInputState>(library, "retro_set_input_state");
        init = loadFunction<Init>(library, "retro_init");
        deinit = loadFunction<Deinit>(library, "retro_deinit");
        loadGame = loadFunction<LoadGame>(library, "retro_load_game");
        run = loadFunction<Run>(library, "retro_run");
        unloadGame = loadFunction<UnloadGame>(library, "retro_unload_game");
        getMemoryData = loadFunction<GetMemoryData>(library, "retro_get_memory_data");
        getMemorySize = loadFunction<GetMemorySize>(library, "retro_get_memory_size");
    }

    ~CoreApi() {
        if (library)
            dlclose(library);
    }

    void *library = nullptr;
    SetEnvironment setEnvironment = nullptr;
    SetVideoRefresh setVideoRefresh = nullptr;
    SetAudioSample setAudioSample = nullptr;
    SetAudioSampleBatch setAudioSampleBatch = nullptr;
    SetInputPoll setInputPoll = nullptr;
    SetInputState setInputState = nullptr;
    Init init = nullptr;
    Deinit deinit = nullptr;
    LoadGame loadGame = nullptr;
    Run run = nullptr;
    UnloadGame unloadGame = nullptr;
    GetMemoryData getMemoryData = nullptr;
    GetMemorySize getMemorySize = nullptr;
};

std::vector<uint8_t> readBinary(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("cannot open ROM: " + path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>());
}

void writeRgb(const std::string &path) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        throw std::runtime_error("cannot create frame output: " + path);

    for (uint32_t pixel : lastFrame) {
        const uint8_t rgb[] = {
            static_cast<uint8_t>((pixel >> 16) & 0xff),
            static_cast<uint8_t>((pixel >> 8) & 0xff),
            static_cast<uint8_t>(pixel & 0xff),
        };
        file.write(reinterpret_cast<const char *>(rgb), sizeof(rgb));
    }
    if (!file)
        throw std::runtime_error("failed writing frame output: " + path);
}

void writeSaveRam(const std::string &path, const uint8_t *data, size_t size) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        throw std::runtime_error("cannot create save RAM output: " + path);
    if (data && size > 0)
        file.write(reinterpret_cast<const char *>(data), size);
    if (!file)
        throw std::runtime_error("failed writing save RAM output: " + path);
}

unsigned parseFrames(const char *value) {
    size_t consumed = 0;
    const unsigned long frames = std::stoul(value, &consumed);
    if (value[consumed] != '\0' || frames == 0 || frames > 1000000)
        throw std::runtime_error("frame count must be between 1 and 1000000");
    return static_cast<unsigned>(frames);
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 6) {
        std::cerr << "Usage: " << argv[0]
                  << " CORE.so ROM.gb FRAMES OUTPUT.rgb OUTPUT.srm\n";
        return 2;
    }

    try {
        const unsigned frames = parseFrames(argv[3]);
        const std::vector<uint8_t> rom = readBinary(argv[2]);
        CoreApi core(argv[1]);

        core.setEnvironment(environmentCallback);
        core.setVideoRefresh(videoRefreshCallback);
        core.setAudioSample(nullptr);
        core.setAudioSampleBatch(audioSampleBatchCallback);
        core.setInputPoll(inputPollCallback);
        core.setInputState(inputStateCallback);
        core.init();

        retro_game_info game{};
        game.data = rom.data();
        game.size = rom.size();
        game.path = argv[2];
        if (!core.loadGame(&game))
            throw std::runtime_error("core rejected ROM: " + std::string(argv[2]));

        for (unsigned frame = 0; frame < frames; ++frame)
            core.run();

        if (!callbackError.empty())
            throw std::runtime_error(callbackError);
        if (!hasVideoFrame || videoFrameCount != frames)
            throw std::runtime_error("core did not publish one video frame per run");

        writeRgb(argv[4]);
        const size_t saveRamSize = core.getMemorySize(RETRO_MEMORY_SAVE_RAM);
        const auto *saveRam = static_cast<const uint8_t *>(
            core.getMemoryData(RETRO_MEMORY_SAVE_RAM));
        if (saveRamSize > 0 && !saveRam)
            throw std::runtime_error("core reported save RAM size without a buffer");
        writeSaveRam(argv[5], saveRam, saveRamSize);
        std::cout << "frames_requested=" << frames
                  << " video_frames=" << videoFrameCount
                  << " audio_frames=" << audioFrameCount
                  << " save_ram_bytes=" << saveRamSize
                  << " width=" << WIDTH << " height=" << HEIGHT << '\n';

        core.unloadGame();
        core.deinit();
    } catch (const std::exception &error) {
        std::cerr << "ROM runner error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
