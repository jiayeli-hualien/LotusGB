#include "libretro.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
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
constexpr unsigned LINK_SUBSYSTEM = 0x100;
constexpr unsigned LINK_SRAM[] = {0x100, 0x102};
constexpr unsigned LINK_RTC[] = {0x101, 0x103};
const retro_subsystem_memory_info LINK_MEMORY[][2] = {
    {{"srm", LINK_SRAM[0]}, {"rtc", LINK_RTC[0]}},
    {{"srm.2", LINK_SRAM[1]}, {"rtc.2", LINK_RTC[1]}},
};
const retro_subsystem_rom_info LINK_ROMS[] = {
    {"Player 1 GBA ROM", "gba", false, false, true, LINK_MEMORY[0], 2},
    {"Player 2 GBA ROM", "gba", false, false, true, LINK_MEMORY[1], 2},
};
const retro_subsystem_info SUBSYSTEMS[] = {
    {"Game Boy Advance Link (2 Players)", "gba_link_2p", LINK_ROMS, 2, LINK_SUBSYSTEM},
    {},
};

retro_environment_t environmentCallback = nullptr;
retro_log_printf_t logCallback = nullptr;
retro_video_refresh_t videoCallback = nullptr;
retro_audio_sample_t audioCallback = nullptr;
retro_audio_sample_batch_t audioBatchCallback = nullptr;
retro_input_poll_t inputPollCallback = nullptr;
retro_input_state_t inputStateCallback = nullptr;
std::unique_ptr<GbaSystem> gbaSystem;
std::unique_ptr<GbaSystem> linkChild;
std::vector<uint8_t> loadedRom;
std::vector<uint8_t> loadedBios;
std::array<uint32_t, 2 * PIXELS> xrgbFrame{};
std::array<uint16_t, 2 * PIXELS> rgb1555Frame{};
bool useXrgb8888 = false;
bool coreFaulted = false;

enum class BiosStartupMode {
    Automatic,
    RequireExternal,
    SkipExternal,
};

constexpr const char *BIOS_MODE_OPTION = "lotusgba_bios_mode";
constexpr const char *LINK_DEVICE_OPTION = "lotusgba_link_device";
enum class LinkDevice : uint8_t { WirelessAdapter, LinkCable };
LinkDevice loadedLinkDevice = LinkDevice::WirelessAdapter;

void registerCoreOptions() {
    if (!environmentCallback)
        return;
    retro_variable options[] = {
        {BIOS_MODE_OPTION,
         "GBA BIOS startup (restart content to apply); Automatic|Use BIOS|Skip BIOS"},
        {LINK_DEVICE_OPTION,
         "Link device (restart content to apply); Wireless Adapter|Link Cable"},
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

LinkDevice readLinkDevice() {
    retro_variable option{LINK_DEVICE_OPTION, nullptr};
    if (!environmentCallback || !environmentCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &option) ||
        !option.value || std::string(option.value) == "Wireless Adapter")
        return LinkDevice::WirelessAdapter;
    if (std::string(option.value) == "Link Cable") return LinkDevice::LinkCable;
    throw std::runtime_error("Unknown GBA link device option");
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
    const unsigned width = linkChild ? 2 * WIDTH : WIDTH;
    const auto draw = [width](const GbaSystem &source, unsigned screenOffset) {
        const auto &pixels = source.frameBuffer();
        for (unsigned y = 0; y < HEIGHT; ++y) {
            for (unsigned x = 0; x < WIDTH; ++x) {
                const uint16_t color = pixels[y * WIDTH + x];
                const size_t destination = y * width + x + screenOffset;
                if (useXrgb8888) {
                    const uint32_t red = expand5(color);
                    const uint32_t green = expand5(color >> 5);
                    const uint32_t blue = expand5(color >> 10);
                    xrgbFrame[destination] = (red << 16) | (green << 8) | blue;
                } else {
                    const uint16_t red = color & 0x1F;
                    const uint16_t green = (color >> 5) & 0x1F;
                    const uint16_t blue = (color >> 10) & 0x1F;
                    rgb1555Frame[destination] =
                        static_cast<uint16_t>((red << 10) | (green << 5) | blue);
                }
            }
        }
    };
    draw(*gbaSystem, 0);
    if (linkChild)
        draw(*linkChild, WIDTH);
    if (useXrgb8888)
        videoCallback(xrgbFrame.data(), width, HEIGHT, width * sizeof(uint32_t));
    else
        videoCallback(rgb1555Frame.data(), width, HEIGHT, width * sizeof(uint16_t));
}

void submitAudio() {
    if (!gbaSystem)
        return;
    // Keep player 1 audible and drain player 2's queue each frame.
    if (linkChild)
        linkChild->takeAudioSamples();
    const std::vector<int16_t> samples = gbaSystem->takeAudioSamples();
    const size_t frames = samples.size() / 2;
    if (audioBatchCallback) {
        audioBatchCallback(samples.data(), frames);
    } else if (audioCallback) {
        for (size_t i = 0; i < frames; ++i)
            audioCallback(samples[i * 2], samples[i * 2 + 1]);
    }
}

uint16_t readPressedButtons(unsigned port) {
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
        if (inputStateCallback(port, RETRO_DEVICE_JOYPAD, 0, binding.retroId))
            pressed |= binding.gbaMask;
    }
    return pressed;
}

constexpr char NETPACKET_PROTOCOL_VERSION[] = "LotusGBA-pokemon-link-v16";
constexpr uint8_t PACKET_MAGIC[] = {'L', 'G', 'B', 'A'};
enum class PacketType : uint8_t {
    Startup, Ready, TransferStart, TransferWord, FrameProgress, DataBlock,
    RfuAdvertisement, RfuConnectionRequest, RfuConnectionResult, RfuData,
    RfuDataAccepted, RfuDisconnect, DataBlockAccepted
};

// Grouped commands retain a bounded immutable pipeline. Transport credits
// mean adapter admission; the final radio fence additionally waits for all
// ordered replies. Neither kind of credit synthesizes a guest NI ACK.
constexpr size_t RFU_DATA_WINDOW = LOTUSGBA::GbaRfuDataChannel::MAX_QUEUED_MESSAGES;
constexpr size_t DATA_BLOCK_WINDOW = 32;

void netpacketPoll();
void clearRfuSendWindow();

struct RemoteNetplay {
    retro_netpacket_send_t send = nullptr;
    retro_netpacket_poll_receive_t pollReceive = nullptr;
    uint16_t localId = 0;
    uint16_t peerId = 0;
    bool connected = false;
    bool failed = false;
    bool sentStartup = false;
    bool receivedStartup = false;
    bool linkConfigured = false;
    bool rfuConfigured = false;
    bool peerReadyKnown = false;
    bool peerReady = false;
    bool localReadyKnown = false;
    bool localReady = false;
    int64_t epoch = 0;
    size_t peerSaveSize = 0;
    size_t peerRtcSize = 0;
    uint64_t peerReadyFrame = 0;
    uint64_t peerFrame = 0;
    bool sioStateKnown = false;
    uint16_t lastSioControl = 0;
    uint16_t lastSioRc = 0;
    std::deque<uint32_t> inFlightRfuData;
    std::deque<uint32_t> inFlightDataBlocks;
    struct DeferredRfuData {
        uint32_t sequence;
        uint8_t payloadBytes;
        std::vector<uint32_t> frameWords;
    };
    std::deque<DeferredRfuData> deferredRfuData;
    uint32_t expectedRfuDataAccepted = 1;
    struct DeferredDataBlock {
        uint32_t sequence;
        GbaSystem::MultiplayerDataWords words;
        uint16_t checksum;
    };
    std::deque<DeferredDataBlock> deferredDataBlocks;
    uint32_t expectedDataBlockAccepted = 1;
    unsigned sioStateLogCount = 0;
    uint64_t packetsSent = 0;
    uint64_t bytesSent = 0;
    uint64_t packetsReceived = 0;
    uint64_t bytesReceived = 0;
    uint64_t transferStartsSent = 0;
    uint64_t transferStartsReceived = 0;
    uint64_t transferWordsSent = 0;
    uint64_t transferWordsReceived = 0;
    uint64_t dataBlocksSent = 0;
    uint64_t dataBlocksReceived = 0;
    uint64_t rfuMessagesSent = 0;
    uint64_t rfuMessagesReceived = 0;
    std::chrono::steady_clock::time_point guestFrameMeasurementStart{};
    bool guestFrameMeasurementStarted = false;
    std::chrono::steady_clock::time_point guestFpsWindowStart{};
    uint64_t guestFpsWindowFrameStart = 0;
    bool guestFpsWindowStarted = false;
    bool statisticsLogged = false;

    bool host() const { return localId == 0; }
};

RemoteNetplay net;

void logRemoteSioState() {
    if (!net.linkConfigured || !gbaSystem)
        return;
    constexpr uint32_t SIOCNT = 0x04000128;
    constexpr uint32_t RCNT = 0x04000134;
    const uint16_t control = gbaSystem->read16(SIOCNT);
    const uint16_t rc = gbaSystem->read16(RCNT);
    if (net.sioStateKnown && control == net.lastSioControl &&
        rc == net.lastSioRc)
        return;
    net.sioStateKnown = true;
    net.lastSioControl = control;
    net.lastSioRc = rc;
    if (net.sioStateLogCount++ >= 128)
        return;
    char message[160];
    std::snprintf(message, sizeof(message),
                  "GBA Netplay SIO state: mode=%u SIOCNT=%04X RCNT=%04X data=%08X send=%04X",
                  (control >> 12) & 3, control, rc,
                  gbaSystem->read32(0x04000120),
                  gbaSystem->read16(0x0400012A));
    logMessage(RETRO_LOG_INFO, message);
}

void logRemoteGuestFrameRate() {
    if (!net.linkConfigured || !gbaSystem)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (!net.guestFpsWindowStarted) {
        net.guestFpsWindowStart = now;
        net.guestFpsWindowFrameStart = gbaSystem->remoteGuestFrameCount();
        net.guestFpsWindowStarted = true;
        return;
    }
    const double elapsed = std::chrono::duration<double>(
        now - net.guestFpsWindowStart).count();
    if (elapsed < 5.0)
        return;
    const uint64_t frames = gbaSystem->remoteGuestFrameCount() -
                            net.guestFpsWindowFrameStart;
    char message[160];
    std::snprintf(message, sizeof(message),
                  "GBA Netplay guest throughput: frames=%llu seconds=%.2f fps=%.2f",
                  static_cast<unsigned long long>(frames), elapsed,
                  static_cast<double>(frames) / elapsed);
    logMessage(RETRO_LOG_INFO, message);
    net.guestFpsWindowStart = now;
    net.guestFpsWindowFrameStart = gbaSystem->remoteGuestFrameCount();
}

uint64_t romFingerprint() {
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t byte : loadedRom)
        hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}

void appendInteger(std::vector<uint8_t> &packet, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i)
        packet.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

uint64_t readInteger(const uint8_t *data, unsigned bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i)
        value |= uint64_t{data[i]} << (i * 8);
    return value;
}

std::vector<uint8_t> makePacket(PacketType type) {
    return {PACKET_MAGIC[0], PACKET_MAGIC[1], PACKET_MAGIC[2],
            PACKET_MAGIC[3], static_cast<uint8_t>(type)};
}

void sendPacket(const std::vector<uint8_t> &packet) {
    if (!net.send || !net.connected)
        throw std::runtime_error("GBA Netplay packet sender is not connected");
    net.send(RETRO_NETPACKET_RELIABLE | RETRO_NETPACKET_FLUSH_HINT,
             packet.data(), packet.size(), net.peerId);
    ++net.packetsSent;
    net.bytesSent += packet.size();
    if (packet.size() > 4) {
        switch (static_cast<PacketType>(packet[4])) {
        case PacketType::TransferStart: ++net.transferStartsSent; break;
        case PacketType::TransferWord: ++net.transferWordsSent; break;
        case PacketType::DataBlock: ++net.dataBlocksSent; break;
        case PacketType::DataBlockAccepted: break;
        case PacketType::RfuAdvertisement:
        case PacketType::RfuConnectionRequest:
        case PacketType::RfuConnectionResult:
        case PacketType::RfuData: {
            ++net.rfuMessagesSent;
            if (net.rfuMessagesSent <= 32 || net.rfuMessagesSent % 256 == 0) {
                char message[128];
                std::snprintf(message, sizeof(message),
                              "GBA Netplay RFU packet sent: type=%u size=%zu count=%llu",
                              packet[4], packet.size(),
                              static_cast<unsigned long long>(net.rfuMessagesSent));
                logMessage(RETRO_LOG_INFO, message);
            }
            break;
        }
        default: break;
        }
    }
}

void logNetplayStatistics(const char *event) {
    if (net.statisticsLogged ||
        (net.packetsSent == 0 && net.packetsReceived == 0))
        return;
    net.statisticsLogged = true;
    const uint64_t guestFrames = gbaSystem
        ? gbaSystem->remoteGuestFrameCount() : 0;
    double guestFps = 0.0;
    if (net.guestFrameMeasurementStarted) {
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() -
            net.guestFrameMeasurementStart).count();
        if (elapsed > 0.0)
            guestFps = static_cast<double>(guestFrames) / elapsed;
    }
    char message[512];
    std::snprintf(
        message, sizeof(message),
        "GBA Netplay statistics (%s, %s): guest_frames=%llu guest_fps=%.2f packets_tx=%llu bytes_tx=%llu packets_rx=%llu bytes_rx=%llu sio_start_tx=%llu sio_start_rx=%llu sio_word_tx=%llu sio_word_rx=%llu data_blocks_tx=%llu data_blocks_rx=%llu",
        event, net.host() ? "host" : "client",
        static_cast<unsigned long long>(guestFrames), guestFps,
        static_cast<unsigned long long>(net.packetsSent),
        static_cast<unsigned long long>(net.bytesSent),
        static_cast<unsigned long long>(net.packetsReceived),
        static_cast<unsigned long long>(net.bytesReceived),
        static_cast<unsigned long long>(net.transferStartsSent),
        static_cast<unsigned long long>(net.transferStartsReceived),
        static_cast<unsigned long long>(net.transferWordsSent),
        static_cast<unsigned long long>(net.transferWordsReceived),
        static_cast<unsigned long long>(net.dataBlocksSent),
        static_cast<unsigned long long>(net.dataBlocksReceived));
    logMessage(RETRO_LOG_INFO, message);
}

void failNetplay(const char *reason) {
    if (net.failed)
        return;
    net.failed = true;
    logMessage(RETRO_LOG_ERROR, reason);
    logNetplayStatistics("failed");
    if (net.rfuConfigured && gbaSystem)
        gbaSystem->disconnectRemoteRfuLink();
    else if (net.linkConfigured && gbaSystem)
        gbaSystem->setRemoteMultiplayerConnected(false);
}

void sendStartup() {
    const auto save = gbaSystem->getSaveRamStorage();
    const auto rtc = gbaSystem->getRtcStorage();
    auto packet = makePacket(PacketType::Startup);
    appendInteger(packet, romFingerprint(), 8);
    appendInteger(packet, static_cast<uint64_t>(net.epoch), 8);
    appendInteger(packet, save ? save->size() : 0, 4);
    appendInteger(packet, rtc ? rtc->size() : 0, 1);
    packet.push_back(static_cast<uint8_t>(loadedLinkDevice));
    sendPacket(packet);
    net.sentStartup = true;
}

void receiveStartup(const uint8_t *data, size_t size) {
    if (size != 22 || net.receivedStartup ||
        readInteger(data, 8) != romFingerprint())
        throw std::runtime_error("GBA Netplay requires the same GBA ROM on both peers");
    if (data[21] != static_cast<uint8_t>(loadedLinkDevice))
        throw std::runtime_error("GBA Netplay link device mismatch; select the same device on both peers");
    const uint64_t epoch = readInteger(data + 8, 8);
    // Restrict the shared startup timestamp to the cartridge's supported era.
    if (epoch < 946684800 || epoch >= 4102444800 ||
        (net.host() && epoch != static_cast<uint64_t>(net.epoch)))
        throw std::runtime_error("Invalid GBA Netplay session clock");
    net.epoch = static_cast<int64_t>(epoch);
    net.peerSaveSize = static_cast<size_t>(readInteger(data + 16, 4));
    net.peerRtcSize = data[20];
    const size_t localSaveSize = gbaSystem->getSaveRamSize();
    const auto isEepromSize = [](size_t size) {
        return size == GbaSystem::EEPROM512_SIZE || size == GbaSystem::EEPROM_SIZE;
    };
    if ((net.peerSaveSize != localSaveSize &&
         !(isEepromSize(net.peerSaveSize) && isEepromSize(localSaveSize))) ||
        net.peerRtcSize != gbaSystem->getRtcSize())
        throw std::runtime_error("Invalid GBA Netplay cartridge storage size");
    net.receivedStartup = true;
}

[[maybe_unused]] void sendRemoteReady(bool ready) {
    if (!net.localReadyKnown || net.localReady != ready) {
        logMessage(RETRO_LOG_INFO,
                   ready ? "GBA Netplay local SIO entered multiplayer mode"
                         : "GBA Netplay local SIO left multiplayer mode");
        net.localReadyKnown = true;
        net.localReady = ready;
    }
    auto packet = makePacket(PacketType::Ready);
    packet.push_back(ready ? 1 : 0);
    appendInteger(packet, gbaSystem ? gbaSystem->remoteFrameCount() : 0, 8);
    sendPacket(packet);
}

[[maybe_unused]] void sendRemoteFrameProgress(uint64_t frame) {
    auto packet = makePacket(PacketType::FrameProgress);
    appendInteger(packet, frame, 8);
    sendPacket(packet);
}

[[maybe_unused]] void sendRemoteTransferStart(uint32_t sequence,
                             uint16_t parentWord, uint8_t baudIndex,
                             uint64_t elapsedCycles) {
    if (sequence <= 32 || sequence % 256 == 0) {
        char message[128];
        std::snprintf(message, sizeof(message),
                      "GBA Netplay SIO start sent: seq=%u word=%04X baud=%u",
                      sequence, parentWord, baudIndex);
        logMessage(RETRO_LOG_INFO, message);
    }
    auto packet = makePacket(PacketType::TransferStart);
    appendInteger(packet, sequence, 4);
    appendInteger(packet, parentWord, 2);
    packet.push_back(baudIndex);
    appendInteger(packet, elapsedCycles, 8);
    sendPacket(packet);
}

[[maybe_unused]] void sendRemoteTransferWord(uint32_t sequence,
                                             uint16_t childWord) {
    if (sequence <= 32 || sequence % 256 == 0) {
        char message[128];
        std::snprintf(message, sizeof(message),
                      "GBA Netplay SIO reply sent: seq=%u word=%04X",
                      sequence, childWord);
        logMessage(RETRO_LOG_INFO, message);
    }
    auto packet = makePacket(PacketType::TransferWord);
    appendInteger(packet, sequence, 4);
    appendInteger(packet, childWord, 2);
    sendPacket(packet);
}

[[maybe_unused]] bool sendRemoteDataBlock(
    uint32_t sequence, const GbaSystem::MultiplayerDataWords &words,
    uint16_t checksum) {
    if (net.inFlightDataBlocks.size() >= DATA_BLOCK_WINDOW)
        return false;
    if (sequence <= 16 || sequence % 256 == 0) {
        char message[128];
        std::snprintf(message, sizeof(message),
                      "GBA Netplay Emerald block sent: seq=%u checksum=%04X cmd=%04X",
                      sequence, checksum, words[0]);
        logMessage(RETRO_LOG_INFO, message);
    }
    auto packet = makePacket(PacketType::DataBlock);
    appendInteger(packet, sequence, 4);
    for (uint16_t word : words)
        appendInteger(packet, word, 2);
    appendInteger(packet, checksum, 2);
    net.inFlightDataBlocks.push_back(sequence);
    sendPacket(packet);
    return true;
}

void sendDataBlockAccepted(uint32_t sequence) {
    auto packet = makePacket(PacketType::DataBlockAccepted);
    appendInteger(packet, sequence, 4);
    sendPacket(packet);
}

void receiveDataBlockAccepted(const uint8_t *data, size_t size) {
    if (size != 4 || !net.linkConfigured)
        throw std::runtime_error("Invalid GBA Netplay data-block admission ACK");
    const uint32_t sequence = static_cast<uint32_t>(readInteger(data, 4));
    if (sequence < net.expectedDataBlockAccepted)
        return;
    if (net.inFlightDataBlocks.empty() ||
        net.inFlightDataBlocks.front() != sequence ||
        sequence != net.expectedDataBlockAccepted)
        throw std::runtime_error("Unexpected GBA Netplay data-block admission ACK");
    net.inFlightDataBlocks.pop_front();
    ++net.expectedDataBlockAccepted;
}

void drainDeferredDataBlocks();

void sendRfuAdvertisement(uint32_t sequence, uint16_t id,
                          const GbaSystem::RfuAdvertisement &data) {
    auto packet = makePacket(PacketType::RfuAdvertisement);
    appendInteger(packet, sequence, 4);
    appendInteger(packet, id, 2);
    for (uint32_t word : data)
        appendInteger(packet, word, 4);
    sendPacket(packet);
}

void sendRfuConnectionRequest(uint32_t sequence, uint16_t sourceId,
                              uint16_t targetId) {
    auto packet = makePacket(PacketType::RfuConnectionRequest);
    appendInteger(packet, sequence, 4);
    appendInteger(packet, sourceId, 2);
    appendInteger(packet, targetId, 2);
    sendPacket(packet);
}

void sendRfuConnectionResult(uint32_t sequence, uint16_t targetId,
                             uint8_t slot, bool accepted) {
    auto packet = makePacket(PacketType::RfuConnectionResult);
    appendInteger(packet, sequence, 4);
    appendInteger(packet, targetId, 2);
    packet.push_back(slot);
    packet.push_back(accepted ? 1 : 0);
    sendPacket(packet);
}

bool sendRfuData(uint32_t sequence, uint8_t payloadBytes,
                 const std::vector<uint32_t> &frameWords) {
    if (payloadBytes > 127 ||
        frameWords.size() != (payloadBytes + 3) / 4 ||
        frameWords.size() + 1 >
            LOTUSGBA::GbaRfuRequestDecoder::MAX_PAYLOAD_WORDS)
        throw std::runtime_error("GBA RFU message exceeds adapter capacity");
    if (net.inFlightRfuData.size() >= RFU_DATA_WINDOW)
        return false;
    auto packet = makePacket(PacketType::RfuData);
    appendInteger(packet, sequence, 4);
    packet.push_back(payloadBytes);
    packet.push_back(static_cast<uint8_t>(frameWords.size()));
    for (uint32_t word : frameWords)
        appendInteger(packet, word, 4);
    net.inFlightRfuData.push_back(sequence);
    sendPacket(packet);
    return true;
}

void clearRfuSendWindow() {
    uint32_t nextExpected = net.expectedRfuDataAccepted;
    if (!net.inFlightRfuData.empty())
        nextExpected = std::max(nextExpected,
                                net.inFlightRfuData.back() + 1);
    net.inFlightRfuData.clear();
    net.expectedRfuDataAccepted = nextExpected;
}

void sendRfuDisconnect();
void sendRfuDataAccepted(uint32_t sequence);

void drainDeferredRfuData() {
    if (!gbaSystem || !net.rfuConfigured)
        return;
    while (!net.deferredRfuData.empty()) {
        auto &pending = net.deferredRfuData.front();
        const auto result = gbaSystem->receiveRemoteRfuData(
            pending.sequence, pending.payloadBytes, pending.frameWords);
        if (result == GbaSystem::RfuDataReceiveResult::QueueFull)
            return;
        if (result == GbaSystem::RfuDataReceiveResult::DroppedDisconnected) {
            sendRfuDataAccepted(pending.sequence);
            net.deferredRfuData.pop_front();
            continue;
        }
        if (result == GbaSystem::RfuDataReceiveResult::Accepted) {
            net.deferredRfuData.pop_front();
            continue;
        }

        logMessage(RETRO_LOG_ERROR,
                   "GBA RFU deferred receive became invalid; disconnecting RFU session");
        net.deferredRfuData.clear();
        sendRfuDisconnect();
        gbaSystem->receiveRemoteRfuDisconnect();
        return;
    }
}

void sendRfuDataAccepted(uint32_t sequence) {
    auto packet = makePacket(PacketType::RfuDataAccepted);
    appendInteger(packet, sequence, 4);
    sendPacket(packet);
}

void sendRfuDisconnect() {
    clearRfuSendWindow();
    net.deferredRfuData.clear();
    sendPacket(makePacket(PacketType::RfuDisconnect));
}

void receiveRfuDataAccepted(const uint8_t *data, size_t size) {
    if (size != 4 || !net.rfuConfigured)
        throw std::runtime_error("Invalid GBA RFU data-accepted message");
    const uint32_t sequence = static_cast<uint32_t>(readInteger(data, 4));
    if (sequence < net.expectedRfuDataAccepted)
        return; // Late/duplicate admission ACK from a cleared RFU send window.
    if (net.inFlightRfuData.empty() ||
        net.inFlightRfuData.front() != sequence ||
        sequence != net.expectedRfuDataAccepted)
        throw std::runtime_error("Unexpected GBA RFU data-accepted sequence");
    net.inFlightRfuData.pop_front();
    ++net.expectedRfuDataAccepted;

}

bool hasEmeraldRfuGameCode() {
    constexpr size_t GAME_CODE_OFFSET = 0xAC;
    constexpr char GAME_CODE[] = {'B', 'P', 'E'};
    return loadedRom.size() > GAME_CODE_OFFSET + sizeof(GAME_CODE) &&
        (loadedRom[GAME_CODE_OFFSET + 3] == 'J' ||
         loadedRom[GAME_CODE_OFFSET + 3] == 'E') &&
        std::equal(std::begin(GAME_CODE), std::end(GAME_CODE),
                   loadedRom.begin() + GAME_CODE_OFFSET);
}

void startRemoteEmulation() {
    const bool cable = loadedLinkDevice == LinkDevice::LinkCable;
    const bool englishEmerald = loadedRom.size() > 0xAF &&
        std::equal(loadedRom.begin() + 0xAC, loadedRom.begin() + 0xB0, "BPEE");
    if ((cable && !englishEmerald) || (!cable && !hasEmeraldRfuGameCode())) {
        failNetplay(cable ? "Pokemon cable profile currently supports English Emerald only"
                         : "Wireless Adapter requires a supported Emerald RFU profile");
        return;
    }
    auto local = std::make_unique<GbaSystem>(
        loadedRom, loadedBios, gbaSystem->getSaveRamStorage(),
        gbaSystem->getRtcStorage(), net.epoch);
    gbaSystem = std::move(local);
    gbaSystem->setRfuAdapterEnabled(!cable);
    if (cable) {
        gbaSystem->configureRemoteMultiplayerLink(!net.host(), true,
            sendRemoteTransferStart, sendRemoteTransferWord, sendRemoteReady,
            sendRemoteFrameProgress, {}, sendRemoteDataBlock);
        gbaSystem->setRemoteMultiplayerBufferedProtocol(true);
        gbaSystem->setRemoteMultiplayerEventPump(netpacketPoll);
        net.linkConfigured = true;
        net.guestFrameMeasurementStart = std::chrono::steady_clock::now();
        net.guestFrameMeasurementStarted = true;
        logMessage(RETRO_LOG_INFO, "GBA Netplay started buffered Pokemon cable (English Emerald)");
        return;
    }
    const auto rfuId = [](uint16_t clientId) {
        return clientId == 0xFFFF ? uint16_t{1}
                                  : static_cast<uint16_t>(clientId + 1);
    };
    gbaSystem->configureRemoteRfuLink(
        rfuId(net.localId), rfuId(net.peerId), sendRfuAdvertisement,
        sendRfuConnectionRequest, sendRfuData, sendRfuDataAccepted,
        sendRfuDisconnect, GbaSystem::RfuCompletionMode::AsyncAdapter);
    gbaSystem->setRemoteRfuEventPump(netpacketPoll);
    const char *detailedTrace = std::getenv("LOTUSGBA_RFU_TRACE");
    gbaSystem->setRfuDiagnosticLogger([](const std::string &message) {
        logMessage(RETRO_LOG_INFO, message.c_str());
    }, detailedTrace && std::string(detailedTrace) == "1");
    net.guestFrameMeasurementStart = std::chrono::steady_clock::now();
    net.guestFrameMeasurementStarted = true;
    net.linkConfigured = true;
    net.rfuConfigured = true;
    logMessage(RETRO_LOG_INFO, loadedBios.empty()
        ? "GBA Netplay startup uses HLE BIOS"
        : "GBA Netplay startup uses the loaded external BIOS");
    logMessage(RETRO_LOG_INFO,
               "GBA Netplay started independent-clock Emerald RFU link");
}

void receiveReady(const uint8_t *data, size_t size) {
    if (size != 9 || data[0] > 1)
        throw std::runtime_error("Invalid GBA Netplay readiness packet");
    const bool ready = data[0] != 0;
    const uint64_t peerFrame = readInteger(data + 1, 8);
    if (!net.peerReadyKnown || net.peerReady != ready)
        logMessage(RETRO_LOG_INFO,
                   ready ? "GBA Netplay peer SIO entered multiplayer mode"
                         : "GBA Netplay peer SIO left multiplayer mode");
    net.peerReadyKnown = true;
    net.peerReady = ready;
    net.peerReadyFrame = peerFrame;
    net.peerFrame = std::max(net.peerFrame, peerFrame);
    if (net.linkConfigured)
        gbaSystem->setRemoteMultiplayerReady(net.peerReady, peerFrame);
    if (net.linkConfigured &&
        !gbaSystem->receiveRemoteMultiplayerFrame(net.peerFrame))
        throw std::runtime_error("Invalid GBA Netplay readiness watermark");
}

void receiveFrameProgress(const uint8_t *data, size_t size) {
    if (size != 8)
        throw std::runtime_error("Invalid GBA Netplay frame watermark");
    const uint64_t frame = readInteger(data, 8);
    if (frame < net.peerFrame)
        throw std::runtime_error("GBA Netplay frame watermark moved backwards");
    net.peerFrame = frame;
    if (net.linkConfigured &&
        !gbaSystem->receiveRemoteMultiplayerFrame(frame))
        throw std::runtime_error("GBA rejected remote frame watermark");
}

void receiveTransferStart(const uint8_t *data, size_t size) {
    if (size != 15 || net.host() || !net.linkConfigured)
        throw std::runtime_error("Unexpected GBA Netplay transfer start");
    const uint32_t sequence = static_cast<uint32_t>(readInteger(data, 4));
    const uint16_t parentWord = static_cast<uint16_t>(readInteger(data + 4, 2));
    const uint8_t baudIndex = data[6];
    const uint64_t elapsedCycles = readInteger(data + 7, 8);
    if (!gbaSystem->receiveRemoteMultiplayerStart(sequence, parentWord,
                                                  baudIndex, elapsedCycles))
        throw std::runtime_error("GBA rejected remote SIO transfer start");
    ++net.transferStartsReceived;
    if (sequence <= 32 || sequence % 256 == 0) {
        char message[128];
        std::snprintf(message, sizeof(message),
                      "GBA Netplay SIO start received: seq=%u word=%04X baud=%u",
                      sequence, parentWord, baudIndex);
        logMessage(RETRO_LOG_INFO, message);
    }
}

void receiveTransferWord(const uint8_t *data, size_t size) {
    if (size != 6 || !net.host() || !net.linkConfigured)
        throw std::runtime_error("Unexpected GBA Netplay transfer word");
    const uint32_t sequence = static_cast<uint32_t>(readInteger(data, 4));
    const uint16_t childWord = static_cast<uint16_t>(readInteger(data + 4, 2));
    if (!gbaSystem->receiveRemoteMultiplayerWord(sequence, childWord))
        throw std::runtime_error("GBA rejected remote SIO transfer word");
    ++net.transferWordsReceived;
    if (sequence <= 32 || sequence % 256 == 0) {
        char message[128];
        std::snprintf(message, sizeof(message),
                      "GBA Netplay SIO reply received: seq=%u word=%04X",
                      sequence, childWord);
        logMessage(RETRO_LOG_INFO, message);
    }
}

void receiveDataBlock(const uint8_t *data, size_t size) {
    constexpr size_t EXPECTED_SIZE = 4 + 8 * 2 + 2;
    if (size != EXPECTED_SIZE || !net.linkConfigured)
        throw std::runtime_error("Invalid GBA Netplay Emerald data block");
    const uint32_t sequence = static_cast<uint32_t>(readInteger(data, 4));
    GbaSystem::MultiplayerDataWords words{};
    for (size_t index = 0; index < words.size(); ++index)
        words[index] = static_cast<uint16_t>(readInteger(data + 4 + index * 2, 2));
    const uint16_t checksum = static_cast<uint16_t>(
        readInteger(data + 4 + words.size() * 2, 2));
    if (!net.deferredDataBlocks.empty()) {
        const uint32_t expected = net.deferredDataBlocks.back().sequence + 1;
        if (sequence != expected ||
            net.deferredDataBlocks.size() >= DATA_BLOCK_WINDOW)
            throw std::runtime_error("GBA Netplay deferred data-block window overflow or sequence gap");
        net.deferredDataBlocks.push_back({sequence, words, checksum});
        return;
    }
    const auto result = gbaSystem->receiveRemoteMultiplayerDataBlockDetailed(
        sequence, words, checksum);
    if (result.reason == GbaSystem::RemoteMultiplayerDataBlockRejectReason::QueueFull) {
        net.deferredDataBlocks.push_back({sequence, words, checksum});
        return;
    }
    if (!result.accepted()) {
        const char *reason = "unknown";
        switch (result.reason) {
        case GbaSystem::RemoteMultiplayerDataBlockRejectReason::None:
            reason = "none";
            break;
        case GbaSystem::RemoteMultiplayerDataBlockRejectReason::LinkNotConfigured:
            reason = "link-not-configured";
            break;
        case GbaSystem::RemoteMultiplayerDataBlockRejectReason::LinkDisconnected:
            reason = "link-disconnected";
            break;
        case GbaSystem::RemoteMultiplayerDataBlockRejectReason::
                 BufferedProtocolNotRequested:
            reason = "buffered-protocol-not-requested";
            break;
        case GbaSystem::RemoteMultiplayerDataBlockRejectReason::SequenceMismatch:
            reason = "sequence-mismatch";
            break;
        case GbaSystem::RemoteMultiplayerDataBlockRejectReason::QueueFull:
            reason = "queue-full";
            break;
        case GbaSystem::RemoteMultiplayerDataBlockRejectReason::ChecksumMismatch:
            reason = "payload-checksum-mismatch";
            break;
        }
        char message[256];
        std::snprintf(message, sizeof(message),
                      "GBA rejected Emerald data block: reason=%s seq=%u expected=%u queue=%zu/%zu active=%u checksum=%04X calculated=%04X",
                      reason, sequence, result.expectedSequence,
                      result.queuedBlocks,
                      GbaSystem::REMOTE_MULTIPLAYER_DATA_BLOCK_QUEUE_LIMIT,
                      result.activeBlock ? 1u : 0u, checksum,
                      result.calculatedChecksum);
        throw std::runtime_error(message);
    }
    sendDataBlockAccepted(sequence);
    ++net.dataBlocksReceived;
    if (sequence <= 16 || sequence % 256 == 0) {
        char message[128];
        std::snprintf(message, sizeof(message),
                      "GBA Netplay Emerald block received: seq=%u checksum=%04X cmd=%04X",
                      sequence, checksum, words[0]);
        logMessage(RETRO_LOG_INFO, message);
    }
}

void drainDeferredDataBlocks() {
    if (!gbaSystem || !net.linkConfigured)
        return;
    while (!net.deferredDataBlocks.empty()) {
        const auto &pending = net.deferredDataBlocks.front();
        const auto result = gbaSystem->receiveRemoteMultiplayerDataBlockDetailed(
            pending.sequence, pending.words, pending.checksum);
        if (result.reason == GbaSystem::RemoteMultiplayerDataBlockRejectReason::QueueFull)
            return;
        if (!result.accepted())
            throw std::runtime_error("GBA rejected deferred Emerald data block");
        sendDataBlockAccepted(pending.sequence);
        ++net.dataBlocksReceived;
        net.deferredDataBlocks.pop_front();
    }
}

void receiveRfuAdvertisement(const uint8_t *data, size_t size) {
    constexpr size_t EXPECTED_SIZE = 4 + 2 + 6 * 4;
    if (size != EXPECTED_SIZE || !net.linkConfigured ||
        !hasEmeraldRfuGameCode())
        throw std::runtime_error("Invalid GBA RFU advertisement");
    const uint32_t sequence = static_cast<uint32_t>(readInteger(data, 4));
    const uint16_t id = static_cast<uint16_t>(readInteger(data + 4, 2));
    GbaSystem::RfuAdvertisement advertisement{};
    for (size_t index = 0; index < advertisement.size(); ++index)
        advertisement[index] = static_cast<uint32_t>(
            readInteger(data + 6 + index * 4, 4));
    if (!gbaSystem->receiveRemoteRfuAdvertisement(sequence, id,
                                                   advertisement))
        throw std::runtime_error("GBA rejected remote RFU advertisement");
    ++net.rfuMessagesReceived;
}

void receiveRfuConnectionRequest(const uint8_t *data, size_t size) {
    if (size != 8 || !net.linkConfigured || !hasEmeraldRfuGameCode())
        throw std::runtime_error("Invalid GBA RFU connection request");
    const uint32_t sequence = static_cast<uint32_t>(readInteger(data, 4));
    const uint16_t sourceId = static_cast<uint16_t>(readInteger(data + 4, 2));
    const uint16_t targetId = static_cast<uint16_t>(readInteger(data + 6, 2));
    const bool accepted = gbaSystem->receiveRemoteRfuConnectionRequest(
        sequence, sourceId, targetId);
    sendRfuConnectionResult(sequence, sourceId, 0, accepted);
    ++net.rfuMessagesReceived;
}

void receiveRfuConnectionResult(const uint8_t *data, size_t size) {
    if (size != 8 || data[7] > 1 || data[6] > 3 || !net.linkConfigured ||
        !hasEmeraldRfuGameCode())
        throw std::runtime_error("Invalid GBA RFU connection result");
    const uint32_t sequence = static_cast<uint32_t>(readInteger(data, 4));
    const uint16_t targetId = static_cast<uint16_t>(readInteger(data + 4, 2));
    if (!gbaSystem->receiveRemoteRfuConnectionResult(
            sequence, targetId, data[6], data[7] != 0))
        throw std::runtime_error("GBA rejected RFU connection result");
    ++net.rfuMessagesReceived;
}

void receiveRfuData(const uint8_t *data, size_t size) {
    if (size < 6 || !net.linkConfigured || !hasEmeraldRfuGameCode())
        throw std::runtime_error("Invalid GBA RFU data message");
    const uint32_t sequence = static_cast<uint32_t>(readInteger(data, 4));
    const uint8_t payloadBytes = data[4];
    const uint8_t wordCount = data[5];
    if (payloadBytes > 127 ||
        wordCount != (payloadBytes + 3) / 4 ||
        wordCount + 1 > LOTUSGBA::GbaRfuRequestDecoder::MAX_PAYLOAD_WORDS ||
        size != 6 + static_cast<size_t>(wordCount) * 4)
        throw std::runtime_error("Invalid GBA RFU data message length");
    std::vector<uint32_t> frameWords;
    frameWords.reserve(wordCount);
    for (size_t index = 0; index < wordCount; ++index)
        frameWords.push_back(static_cast<uint32_t>(
            readInteger(data + 6 + index * 4, 4)));
    if (!net.deferredRfuData.empty()) {
        if (sequence != net.deferredRfuData.back().sequence + 1) {
            throw std::runtime_error("Out-of-order GBA RFU packet behind deferred data");
        }
        if (net.deferredRfuData.size() >= RFU_DATA_WINDOW) {
            logMessage(RETRO_LOG_ERROR,
                       "GBA RFU deferred receive window exhausted; disconnecting RFU session");
            net.deferredRfuData.clear();
            sendRfuDisconnect();
            gbaSystem->receiveRemoteRfuDisconnect();
            ++net.rfuMessagesReceived;
            return;
        }
        net.deferredRfuData.push_back(
            {sequence, payloadBytes, std::move(frameWords)});
        ++net.rfuMessagesReceived;
        return;
    }

    const auto result = gbaSystem->receiveRemoteRfuData(
        sequence, payloadBytes, frameWords);
    if (result == GbaSystem::RfuDataReceiveResult::DroppedDisconnected) {
        // The peer may have sent this before it observed our RFU reset. Drop
        // the stale payload but return its credit so its bounded send window
        // cannot deadlock while the games search for the adapter again.
        sendRfuDataAccepted(sequence);
        ++net.rfuMessagesReceived;
        return;
    }
    if (result != GbaSystem::RfuDataReceiveResult::Accepted) {
        if (result == GbaSystem::RfuDataReceiveResult::QueueFull) {
            net.deferredRfuData.push_back(
                {sequence, payloadBytes, std::move(frameWords)});
            ++net.rfuMessagesReceived;
            return;
        }
        const char *reason = "unknown";
        switch (result) {
        case GbaSystem::RfuDataReceiveResult::LinkDisabled:
            reason = "RFU link is disabled";
            break;
        case GbaSystem::RfuDataReceiveResult::NotConnected:
            reason = "RFU connection is not established";
            break;
        case GbaSystem::RfuDataReceiveResult::DroppedDisconnected:
            break;
        case GbaSystem::RfuDataReceiveResult::UnexpectedSequence:
            reason = "RFU data sequence is out of order";
            break;
        case GbaSystem::RfuDataReceiveResult::PayloadTooLarge:
            reason = "RFU data payload exceeds adapter capacity";
            break;
        case GbaSystem::RfuDataReceiveResult::QueueFull:
            reason = "RFU receive queue is full";
            break;
        case GbaSystem::RfuDataReceiveResult::Accepted:
            break;
        }
        char message[160];
        std::snprintf(message, sizeof(message),
                      "GBA rejected remote RFU data: seq=%u words=%u (%s)",
                      sequence, wordCount, reason);
        throw std::runtime_error(message);
    }
    ++net.rfuMessagesReceived;
}

void receiveRfuDisconnect(const uint8_t *, size_t size) {
    if (size != 0 || !net.rfuConfigured || !gbaSystem)
        throw std::runtime_error("Invalid GBA RFU disconnect message");
    clearRfuSendWindow();
    gbaSystem->receiveRemoteRfuDisconnect();
    net.deferredRfuData.clear();
    ++net.rfuMessagesReceived;
    logMessage(RETRO_LOG_INFO,
               "GBA Netplay peer reset the Emerald RFU adapter link");
}

void netpacketStart(uint16_t clientId, retro_netpacket_send_t send,
                    retro_netpacket_poll_receive_t pollReceive) try {
    if (net.send && net.localId == clientId) {
        net.send = send;
        net.pollReceive = pollReceive;
        return;
    }
    net = {};
    net.localId = clientId;
    net.send = send;
    net.pollReceive = pollReceive;
    net.connected = !net.host();
    logMessage(RETRO_LOG_INFO,
               net.host() ? "GBA Netplay packet transport started as host"
                          : "GBA Netplay packet transport started as client");
    if (linkChild)
        failNetplay("Start GBA Netplay with one ROM per device");
} catch (...) {
    failNetplay("Could not start GBA Netplay");
}

void netpacketStop() {
    logNetplayStatistics("stopped");
    if (net.rfuConfigured && gbaSystem)
        gbaSystem->disconnectRemoteRfuLink();
    else if (net.linkConfigured && gbaSystem)
        gbaSystem->disconnectRemoteMultiplayerLink();
    net = {};
}

void netpacketReceive(const void *data, size_t size, uint16_t clientId) try {
    if (!net.send || !net.connected || net.failed || !gbaSystem || clientId != net.peerId)
        return;
    ++net.packetsReceived;
    net.bytesReceived += size;
    const auto *bytes = static_cast<const uint8_t *>(data);
    if (!bytes || size < 5 || !std::equal(std::begin(PACKET_MAGIC),
                                         std::end(PACKET_MAGIC), bytes))
        throw std::runtime_error("Invalid GBA Netplay packet");
    const auto packetType = static_cast<PacketType>(bytes[4]);
    if (packetType == PacketType::RfuAdvertisement ||
        packetType == PacketType::RfuConnectionRequest ||
        packetType == PacketType::RfuConnectionResult ||
        packetType == PacketType::RfuData ||
        packetType == PacketType::RfuDataAccepted ||
        packetType == PacketType::RfuDisconnect) {
        const uint64_t count = net.rfuMessagesReceived + 1;
        if (count <= 32 || count % 256 == 0) {
            char message[128];
            std::snprintf(message, sizeof(message),
                          "GBA Netplay RFU packet received: type=%u size=%zu count=%llu",
                          bytes[4], size,
                          static_cast<unsigned long long>(count));
            logMessage(RETRO_LOG_INFO, message);
        }
    }
    switch (packetType) {
    case PacketType::Startup: receiveStartup(bytes + 5, size - 5); break;
    case PacketType::Ready: receiveReady(bytes + 5, size - 5); break;
    case PacketType::TransferStart:
        receiveTransferStart(bytes + 5, size - 5);
        break;
    case PacketType::TransferWord:
        receiveTransferWord(bytes + 5, size - 5);
        break;
    case PacketType::FrameProgress:
        receiveFrameProgress(bytes + 5, size - 5);
        break;
    case PacketType::DataBlock:
        receiveDataBlock(bytes + 5, size - 5);
        break;
    case PacketType::DataBlockAccepted:
        receiveDataBlockAccepted(bytes + 5, size - 5);
        break;
    case PacketType::RfuAdvertisement:
        receiveRfuAdvertisement(bytes + 5, size - 5);
        break;
    case PacketType::RfuConnectionRequest:
        receiveRfuConnectionRequest(bytes + 5, size - 5);
        break;
    case PacketType::RfuConnectionResult:
        receiveRfuConnectionResult(bytes + 5, size - 5);
        break;
    case PacketType::RfuData:
        receiveRfuData(bytes + 5, size - 5);
        break;
    case PacketType::RfuDataAccepted:
        receiveRfuDataAccepted(bytes + 5, size - 5);
        break;
    case PacketType::RfuDisconnect:
        receiveRfuDisconnect(bytes + 5, size - 5);
        break;
    default: throw std::runtime_error("Unknown GBA Netplay packet");
    }
} catch (const std::exception &error) {
    failNetplay(error.what());
} catch (...) {
    failNetplay("Could not process GBA Netplay packet");
}

void netpacketPoll() try {
    if (net.send && net.pollReceive && net.connected && !net.failed)
        net.pollReceive();
    drainDeferredRfuData();
    drainDeferredDataBlocks();
    if (net.send && net.connected && !net.failed) {
        if (!net.sentStartup && (net.host() || net.receivedStartup))
            sendStartup();
        if (net.sentStartup && net.receivedStartup && !net.linkConfigured)
            startRemoteEmulation();
    }
} catch (...) {
    failNetplay("Could not poll GBA Netplay");
}

bool netpacketConnected(uint16_t clientId) {
    if (!net.send || !net.host() || !gbaSystem || linkChild ||
        net.connected || net.failed)
        return false;
    net.peerId = clientId;
    net.epoch = static_cast<int64_t>(std::time(nullptr));
    net.connected = true;
    return true;
}

void netpacketDisconnected(uint16_t clientId) {
    if (net.connected && clientId == net.peerId) {
        failNetplay("GBA Netplay peer disconnected");
        if (net.rfuConfigured && gbaSystem)
            gbaSystem->disconnectRemoteRfuLink();
        else if (net.linkConfigured && gbaSystem)
            gbaSystem->setRemoteMultiplayerConnected(false);
    }
}

const retro_netpacket_callback NETPACKET_CALLBACKS = {
    netpacketStart, netpacketReceive, netpacketStop, netpacketPoll,
    netpacketConnected, netpacketDisconnected, NETPACKET_PROTOCOL_VERSION};

} // namespace

extern "C" {

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_set_environment(retro_environment_t callback) {
    environmentCallback = callback;
    if (environmentCallback) {
        bool noGame = false;
        environmentCallback(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noGame);
        registerCoreOptions();
        environmentCallback(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO,
                            const_cast<retro_subsystem_info *>(SUBSYSTEMS));
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
        if (!environmentCallback(
                RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE,
                const_cast<retro_netpacket_callback *>(&NETPACKET_CALLBACKS)))
            logMessage(RETRO_LOG_INFO,
                       "Frontend does not provide the GBA Netpacket interface");
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
    retro_unload_game();
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
    info->geometry.base_width = linkChild ? 2 * WIDTH : WIDTH;
    info->geometry.base_height = HEIGHT;
    info->geometry.max_width = info->geometry.base_width;
    info->geometry.max_height = HEIGHT;
    info->geometry.aspect_ratio = static_cast<float>(info->geometry.base_width) / HEIGHT;
    info->timing.fps = GBA_FPS;
    info->timing.sample_rate = GbaSystem::AUDIO_SAMPLE_RATE;
}

void retro_set_controller_port_device(unsigned, unsigned) {}

void retro_reset(void) {
    if (!gbaSystem)
        return;
    if (net.send) {
        logMessage(RETRO_LOG_WARN, "End GBA Netplay before resetting");
        return;
    }
    try {
        gbaSystem->reset();
        if (linkChild)
            linkChild->reset();
        coreFaulted = false;
    } catch (const std::exception &error) {
        logMessage(RETRO_LOG_ERROR, error.what());
        coreFaulted = true;
    } catch (...) {
        logMessage(RETRO_LOG_ERROR, "Unexpected exception resetting LotusGBA");
        coreFaulted = true;
    }
}

void retro_run(void) {
    if (!gbaSystem || coreFaulted)
        return;
    try {
        netpacketPoll();
        if (inputPollCallback)
            inputPollCallback();
        if (net.send && net.connected && !net.linkConfigured && !net.failed) {
            submitVideo();
            return;
        }
        const auto result = [&]() {
            gbaSystem->setPressedButtons(readPressedButtons(0));
            if (linkChild) {
                linkChild->setPressedButtons(readPressedButtons(1));
                return gbaSystem->runLinkedFrame();
            }
            return gbaSystem->runFrame();
        }();
        logRemoteSioState();
        logRemoteGuestFrameRate();
        if (!result.success) {
            const auto &fault = linkChild && gbaSystem->fault().empty()
                ? linkChild->fault() : gbaSystem->fault();
            logMessage(RETRO_LOG_ERROR, fault.c_str());
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

static bool validGame(const retro_game_info *game) {
    if (!game || !game->data || game->size < 0xC4 || game->size > 32 * 1024 * 1024) {
        logMessage(RETRO_LOG_ERROR, "GBA content must contain at least 0xC4 bytes and be at most 32 MiB");
        return false;
    }
    return true;
}

static std::vector<uint8_t> copyRom(const retro_game_info &game) {
    const auto *begin = static_cast<const uint8_t *>(game.data);
    return {begin, begin + game.size};
}

bool retro_load_game(const retro_game_info *game) {
    if (!validGame(game))
        return false;
    try {
        auto candidate = copyRom(*game);
        auto candidateBios = loadBios(readBiosStartupMode());
        const LinkDevice candidateDevice = readLinkDevice();
        std::shared_ptr<std::vector<uint8_t>> candidateSaveFlash;
        std::shared_ptr<std::vector<uint8_t>> candidateRtc;
        if (gbaSystem && loadedRom == candidate) {
            candidateSaveFlash = gbaSystem->getSaveRamStorage();
            candidateRtc = gbaSystem->getRtcStorage();
        }
        auto candidateSystem = std::make_unique<GbaSystem>(
            candidate, candidateBios, std::move(candidateSaveFlash),
            std::move(candidateRtc));
        candidateSystem->setRfuAdapterEnabled(candidateDevice == LinkDevice::WirelessAdapter);
        linkChild.reset();
        loadedRom = std::move(candidate);
        loadedBios = std::move(candidateBios);
        gbaSystem = std::move(candidateSystem);
        loadedLinkDevice = candidateDevice;
        coreFaulted = false;
        if (net.sentStartup || net.receivedStartup)
            failNetplay("GBA content changed during Netplay. Restart the session.");
    } catch (const std::exception &error) {
        logMessage(RETRO_LOG_ERROR, error.what());
        return false;
    } catch (...) {
        logMessage(RETRO_LOG_ERROR, "Unexpected error loading GBA content");
        return false;
    }
    return true;
}

bool retro_load_game_special(unsigned type, const retro_game_info *games, size_t count) {
    if (type != LINK_SUBSYSTEM || !games || count != 2) {
        logMessage(RETRO_LOG_ERROR, "GBA Link requires two ROMs");
        return false;
    }
    if (!validGame(&games[0]) || !validGame(&games[1]))
        return false;
    try {
        const auto bios = loadBios(readBiosStartupMode());
        const LinkDevice device = readLinkDevice();
        auto parent = std::make_unique<GbaSystem>(copyRom(games[0]), bios);
        auto child = std::make_unique<GbaSystem>(copyRom(games[1]), bios);
        parent->setRfuAdapterEnabled(device == LinkDevice::WirelessAdapter);
        child->setRfuAdapterEnabled(device == LinkDevice::WirelessAdapter);
        parent->connectLinkChild(*child);
        retro_unload_game();
        gbaSystem = std::move(parent);
        linkChild = std::move(child);
        loadedLinkDevice = device;
        return true;
    } catch (const std::exception &error) {
        logMessage(RETRO_LOG_ERROR, error.what());
    } catch (...) {
        logMessage(RETRO_LOG_ERROR, "Unexpected error loading the GBA Link subsystem");
    }
    return false;
}

void retro_unload_game(void) {
    netpacketStop();
    linkChild.reset();
    gbaSystem.reset();
    loadedRom.clear();
    loadedBios.clear();
    coreFaulted = false;
}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
static GbaSystem *systemForMemory(unsigned id) {
    if (id == RETRO_MEMORY_SAVE_RAM || id == RETRO_MEMORY_RTC)
        return linkChild ? nullptr : gbaSystem.get();
    if (!linkChild)
        return nullptr;
    if (id == LINK_SRAM[0] || id == LINK_RTC[0])
        return gbaSystem.get();
    if (id == LINK_SRAM[1] || id == LINK_RTC[1])
        return linkChild.get();
    return nullptr;
}

void *retro_get_memory_data(unsigned id) {
    auto *system = systemForMemory(id);
    if (!system)
        return nullptr;
    return id == RETRO_MEMORY_RTC || id == LINK_RTC[0] || id == LINK_RTC[1]
        ? system->getRtcData() : system->getSaveRamData();
}

size_t retro_get_memory_size(unsigned id) {
    auto *system = systemForMemory(id);
    if (!system)
        return 0;
    return id == RETRO_MEMORY_RTC || id == LINK_RTC[0] || id == LINK_RTC[1]
        ? system->getRtcSize() : system->getSaveRamSize();
}

} // extern "C"
