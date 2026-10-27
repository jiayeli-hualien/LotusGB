#include "libretro.h"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr unsigned WIDTH = 160;
constexpr unsigned HEIGHT = 144;
constexpr size_t PIXEL_COUNT = WIDTH * HEIGHT;

std::array<uint32_t, PIXEL_COUNT> lastFrame{};
std::string systemDirectory;
unsigned activePixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
unsigned videoFrameCount = 0;
size_t audioFrameCount = 0;
bool hasVideoFrame = false;
std::string callbackError;
unsigned currentFrame = 0;
std::ofstream frameSequence;
unsigned captureStartFrame = 0;
unsigned captureEndFrame = 0;
struct InputEvent { unsigned start, end, button; };
std::vector<InputEvent> inputEvents;

bool environmentCallback(unsigned command, void *data) {
    if (command == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY) {
        if (!data)
            return false;
        *static_cast<const char **>(data) =
            systemDirectory.empty() ? nullptr : systemDirectory.c_str();
        return true;
    }
    if (command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) {
        if (!data)
            return false;
        const unsigned requested = *static_cast<unsigned *>(data);
        if (requested != RETRO_PIXEL_FORMAT_0RGB1555 &&
            requested != RETRO_PIXEL_FORMAT_XRGB8888 &&
            requested != RETRO_PIXEL_FORMAT_RGB565)
            return false;
        activePixelFormat = requested;
        return true;
    }
    if (command == RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS)
        return true;
    if (command == RETRO_ENVIRONMENT_GET_VARIABLE) {
        if (!data)
            return false;
        auto *variable = static_cast<retro_variable *>(data);
        if (!variable->key)
            return false;
        const std::string environmentName =
            std::string("LOTUSGB_CORE_OPTION_") + variable->key;
        variable->value = std::getenv(environmentName.c_str());
        return variable->value != nullptr;
    }
    return false;
}

void videoRefreshCallback(const void *data, unsigned width, unsigned height,
                          size_t pitch) {
    ++videoFrameCount;
    const size_t bytesPerPixel =
        activePixelFormat == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
    if (!data || width != WIDTH || height != HEIGHT ||
        pitch < WIDTH * bytesPerPixel) {
        callbackError = "core returned an invalid video frame";
        return;
    }

    const auto *bytes = static_cast<const uint8_t *>(data);
    for (unsigned row = 0; row < HEIGHT; ++row) {
        const uint8_t *sourceRow = bytes + row * pitch;
        for (unsigned column = 0; column < WIDTH; ++column) {
            uint32_t pixel = 0;
            if (activePixelFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
                std::memcpy(&pixel, sourceRow + column * sizeof(uint32_t),
                            sizeof(pixel));
            } else {
                uint16_t packed = 0;
                std::memcpy(&packed, sourceRow + column * sizeof(packed),
                            sizeof(packed));
                if (activePixelFormat == RETRO_PIXEL_FORMAT_0RGB1555) {
                    const uint8_t red = (packed >> 10) & 0x1F;
                    const uint8_t green = (packed >> 5) & 0x1F;
                    const uint8_t blue = packed & 0x1F;
                    pixel = static_cast<uint32_t>(((red << 3) | (red >> 2)) << 16) |
                            static_cast<uint32_t>(((green << 3) | (green >> 2)) << 8) |
                            static_cast<uint32_t>((blue << 3) | (blue >> 2));
                } else {
                    const uint8_t red = (packed >> 11) & 0x1F;
                    const uint8_t green = (packed >> 5) & 0x3F;
                    const uint8_t blue = packed & 0x1F;
                    pixel = static_cast<uint32_t>(((red << 3) | (red >> 2)) << 16) |
                            static_cast<uint32_t>(((green << 2) | (green >> 4)) << 8) |
                            static_cast<uint32_t>((blue << 3) | (blue >> 2));
                }
            }
            lastFrame[row * WIDTH + column] = pixel;
        }
    }
    hasVideoFrame = true;
    const unsigned frameIndex = videoFrameCount - 1;
    if (frameSequence.is_open() && frameIndex >= captureStartFrame &&
        frameIndex < captureEndFrame) {
        for (uint32_t pixel : lastFrame) {
            const uint8_t rgb[] = {
                static_cast<uint8_t>((pixel >> 16) & 0xff),
                static_cast<uint8_t>((pixel >> 8) & 0xff),
                static_cast<uint8_t>(pixel & 0xff),
            };
            frameSequence.write(reinterpret_cast<const char *>(rgb), sizeof(rgb));
        }
        if (!frameSequence)
            callbackError = "failed writing captured frame sequence";
    }
}

size_t audioSampleBatchCallback(const int16_t *, size_t frames) {
    audioFrameCount += frames;
    return frames;
}

void inputPollCallback() {}

int16_t inputStateCallback(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || index != 0)
        return 0;
    for (const auto &event : inputEvents)
        if (event.button == id && currentFrame >= event.start && currentFrame < event.end)
            return 1;
    return 0;
}

void readInputScript(const std::string &path, unsigned frames) {
    std::ifstream file(path);
    if (!file)
        throw std::runtime_error("cannot open input script: " + path);
    const std::array<std::pair<const char *, unsigned>, 8> buttons{{
        {"A", RETRO_DEVICE_ID_JOYPAD_A}, {"B", RETRO_DEVICE_ID_JOYPAD_B},
        {"START", RETRO_DEVICE_ID_JOYPAD_START}, {"SELECT", RETRO_DEVICE_ID_JOYPAD_SELECT},
        {"UP", RETRO_DEVICE_ID_JOYPAD_UP}, {"DOWN", RETRO_DEVICE_ID_JOYPAD_DOWN},
        {"LEFT", RETRO_DEVICE_ID_JOYPAD_LEFT}, {"RIGHT", RETRO_DEVICE_ID_JOYPAD_RIGHT}}};
    std::string line;
    unsigned lineNumber = 0;
    while (std::getline(file, line)) {
        ++lineNumber;
        line = line.substr(0, line.find('#'));
        std::istringstream fields(line);
        fields >> std::ws;
        if (fields.eof()) continue;
        InputEvent event{};
        std::string button, extra;
        if (!(fields >> event.start >> event.end >> button) || fields >> extra ||
            event.start >= event.end || event.end > frames)
            throw std::runtime_error("invalid input interval at line " + std::to_string(lineNumber));
        bool found = false;
        for (const auto &entry : buttons)
            if (button == entry.first) { event.button = entry.second; found = true; }
        if (!found)
            throw std::runtime_error("unknown input button: " + button);
        inputEvents.push_back(event);
    }
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
    struct LibraryCloser {
        void operator()(void *handle) const noexcept {
            if (dlclose(handle) != 0) {
                const char *error = dlerror();
                std::cerr << "ROM runner error closing core: "
                          << (error ? error : "unknown error") << '\n';
            }
        }
    };

    using SetEnvironment = void (*)(retro_environment_t);
    using SetVideoRefresh = void (*)(retro_video_refresh_t);
    using SetAudioSample = void (*)(retro_audio_sample_t);
    using SetAudioSampleBatch = void (*)(retro_audio_sample_batch_t);
    using SetInputPoll = void (*)(retro_input_poll_t);
    using SetInputState = void (*)(retro_input_state_t);
    using Init = void (*)(void);
    using Deinit = void (*)(void);
    using GetSystemInfo = void (*)(retro_system_info *);
    using LoadGame = bool (*)(const retro_game_info *);
    using Run = void (*)(void);
    using UnloadGame = void (*)(void);
    using GetMemoryData = void *(*)(unsigned);
    using GetMemorySize = size_t (*)(unsigned);

    explicit CoreApi(const char *path) {
        library.reset(dlopen(path, RTLD_NOW | RTLD_LOCAL));
        if (!library)
            throw std::runtime_error(std::string("cannot load core: ") +
                                     dlerror());

        setEnvironment = loadFunction<SetEnvironment>(library.get(), "retro_set_environment");
        setVideoRefresh = loadFunction<SetVideoRefresh>(library.get(), "retro_set_video_refresh");
        setAudioSample = loadFunction<SetAudioSample>(library.get(), "retro_set_audio_sample");
        setAudioSampleBatch =
            loadFunction<SetAudioSampleBatch>(library.get(), "retro_set_audio_sample_batch");
        setInputPoll = loadFunction<SetInputPoll>(library.get(), "retro_set_input_poll");
        setInputState = loadFunction<SetInputState>(library.get(), "retro_set_input_state");
        init = loadFunction<Init>(library.get(), "retro_init");
        deinit = loadFunction<Deinit>(library.get(), "retro_deinit");
        getSystemInfo = loadFunction<GetSystemInfo>(library.get(), "retro_get_system_info");
        loadGame = loadFunction<LoadGame>(library.get(), "retro_load_game");
        run = loadFunction<Run>(library.get(), "retro_run");
        unloadGame = loadFunction<UnloadGame>(library.get(), "retro_unload_game");
        getMemoryData = loadFunction<GetMemoryData>(library.get(), "retro_get_memory_data");
        getMemorySize = loadFunction<GetMemorySize>(library.get(), "retro_get_memory_size");
    }

    ~CoreApi() {
        if (gameLoaded)
            unloadGame();
        if (initialized)
            deinit();
    }

    void initialize() {
        retro_system_info systemInfo{};
        getSystemInfo(&systemInfo);
        init();
        initialized = true;
    }

    bool load(const retro_game_info *game) {
        if (!loadGame(game))
            return false;
        gameLoaded = true;
        return true;
    }

    void unload() {
        if (!gameLoaded)
            return;
        unloadGame();
        gameLoaded = false;
    }

    void shutdown() {
        if (!initialized)
            return;
        deinit();
        initialized = false;
    }

    void close() {
        unload();
        shutdown();
        void *handle = library.release();
        if (dlclose(handle) != 0) {
            const char *error = dlerror();
            throw std::runtime_error(std::string("cannot close core: ") +
                                     (error ? error : "unknown error"));
        }
    }

    std::unique_ptr<void, LibraryCloser> library;
    bool initialized = false;
    bool gameLoaded = false;
    SetEnvironment setEnvironment = nullptr;
    SetVideoRefresh setVideoRefresh = nullptr;
    SetAudioSample setAudioSample = nullptr;
    SetAudioSampleBatch setAudioSampleBatch = nullptr;
    SetInputPoll setInputPoll = nullptr;
    SetInputState setInputState = nullptr;
    Init init = nullptr;
    Deinit deinit = nullptr;
    GetSystemInfo getSystemInfo = nullptr;
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

unsigned parseFrameIndex(const char *value) {
    size_t consumed = 0;
    const unsigned long frame = std::stoul(value, &consumed);
    if (value[consumed] != '\0' || frame > 1000000)
        throw std::runtime_error("frame index must be between 0 and 1000000");
    return static_cast<unsigned>(frame);
}

unsigned parseRepetitions(const char *value) {
    size_t consumed = 0;
    const unsigned long repetitions = std::stoul(value, &consumed);
    if (value[consumed] != '\0' || repetitions == 0 || repetitions > 1000)
        throw std::runtime_error("repeat count must be between 1 and 1000");
    return static_cast<unsigned>(repetitions);
}

} // namespace

int main(int argc, char **argv) {
    if (const char *configuredSystemDirectory =
            std::getenv("LOTUSGB_SYSTEM_DIRECTORY")) {
        systemDirectory = configuredSystemDirectory;
        if (!systemDirectory.empty() &&
            !std::filesystem::is_directory(systemDirectory)) {
            std::cerr << "ROM runner error: system directory does not exist: "
                      << systemDirectory << '\n';
            return 2;
        }
    }

    try {
        const char *sequencePath = std::getenv("LOTUSGB_CAPTURE_SEQUENCE");
        const char *captureStart = std::getenv("LOTUSGB_CAPTURE_START");
        const char *captureEnd = std::getenv("LOTUSGB_CAPTURE_END");
        if (sequencePath || captureStart || captureEnd) {
            if (!sequencePath || !captureStart || !captureEnd)
                throw std::runtime_error(
                    "frame capture requires LOTUSGB_CAPTURE_SEQUENCE, "
                    "LOTUSGB_CAPTURE_START, and LOTUSGB_CAPTURE_END");
            captureStartFrame = parseFrameIndex(captureStart);
            captureEndFrame = parseFrameIndex(captureEnd);
            if (captureEndFrame <= captureStartFrame)
                throw std::runtime_error(
                    "capture end must be greater than capture start");
            frameSequence.open(sequencePath, std::ios::binary | std::ios::trunc);
            if (!frameSequence)
                throw std::runtime_error(std::string("cannot create frame sequence: ") +
                                         sequencePath);
        }
    } catch (const std::exception &error) {
        std::cerr << "ROM runner error: " << error.what() << '\n';
        return 2;
    }

    unsigned repetitions = 1;
    int contentArgc = argc;
    try {
        if (argc >= 8 && std::strcmp(argv[argc - 2], "--repeat") == 0) {
            repetitions = parseRepetitions(argv[argc - 1]);
            contentArgc -= 2;
        }
    } catch (const std::exception &error) {
        std::cerr << "ROM runner error: " << error.what() << '\n';
        return 2;
    }
    if (contentArgc < 6 || contentArgc > 8) {
        std::cerr << "Usage: " << argv[0]
                  << " CORE.so ROM.gb FRAMES OUTPUT.rgb OUTPUT.srm"
                  << " [INPUT.txt [RESTORE.srm]] [--repeat COUNT]\n";
        return 2;
    }

    try {
        const unsigned frames = parseFrames(argv[3]);
        if (contentArgc >= 7) readInputScript(argv[6], frames);
        if (contentArgc == 8) {
            for (int output : {4, 5})
                if (std::filesystem::exists(argv[output]) &&
                    std::filesystem::equivalent(argv[7], argv[output]))
                    throw std::runtime_error("output must not overwrite restored save");
        }
        const std::vector<uint8_t> rom = readBinary(argv[2]);
        const std::vector<uint8_t> saved =
            contentArgc == 8 ? readBinary(argv[7]) : std::vector<uint8_t>{};

        retro_game_info game{};
        game.data = rom.data();
        game.size = rom.size();
        game.path = argv[2];
        size_t finalSaveRamSize = 0;
        for (unsigned repetition = 0; repetition < repetitions; ++repetition) {
            videoFrameCount = 0;
            audioFrameCount = 0;
            hasVideoFrame = false;
            activePixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
            callbackError.clear();
            CoreApi core(argv[1]);
            core.setEnvironment(environmentCallback);
            core.setVideoRefresh(videoRefreshCallback);
            core.setAudioSample(nullptr);
            core.setAudioSampleBatch(audioSampleBatchCallback);
            core.setInputPoll(inputPollCallback);
            core.setInputState(inputStateCallback);
            core.initialize();
            if (!core.load(&game))
                throw std::runtime_error("core rejected ROM: " + std::string(argv[2]));

            if (contentArgc == 8) {
                void *ram = core.getMemoryData(RETRO_MEMORY_SAVE_RAM);
                if (!ram || saved.size() != core.getMemorySize(RETRO_MEMORY_SAVE_RAM))
                    throw std::runtime_error("restored save size does not match cartridge RAM");
                std::memcpy(ram, saved.data(), saved.size());
            }
            for (currentFrame = 0; currentFrame < frames; ++currentFrame)
                core.run();

            if (!callbackError.empty())
                throw std::runtime_error(callbackError);
            if (!hasVideoFrame || videoFrameCount != frames)
                throw std::runtime_error("core did not publish one video frame per run");

            if (repetition + 1 == repetitions) {
                writeRgb(argv[4]);
                const size_t saveRamSize = core.getMemorySize(RETRO_MEMORY_SAVE_RAM);
                const auto *saveRam = static_cast<const uint8_t *>(
                    core.getMemoryData(RETRO_MEMORY_SAVE_RAM));
                if (saveRamSize > 0 && !saveRam)
                    throw std::runtime_error("core reported save RAM size without a buffer");
                writeSaveRam(argv[5], saveRam, saveRamSize);
                finalSaveRamSize = saveRamSize;
            }
            core.close();
            if (repetition + 1 == repetitions) {
                std::cout << "frames_requested=" << frames
                          << " video_frames=" << videoFrameCount
                          << " audio_frames=" << audioFrameCount
                          << " save_ram_bytes=" << finalSaveRamSize
                          << " width=" << WIDTH << " height=" << HEIGHT;
                if (repetitions > 1)
                    std::cout << " repetitions=" << repetitions;
                std::cout << '\n';
            }
        }
    } catch (const std::exception &error) {
        std::cerr << "ROM runner error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
