#ifndef LOTUSGBA_GBA_SYSTEM_HPP
#define LOTUSGBA_GBA_SYSTEM_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "arm7tdmi.hpp"
#include "gba_audio.hpp"
#include "gba_cartridge_rtc.hpp"

namespace LOTUSGBA {

class GbaSystem final : public GbaBus {
public:
    static constexpr unsigned SCREEN_WIDTH = 240;
    static constexpr unsigned SCREEN_HEIGHT = 160;
    static constexpr size_t PIXEL_COUNT = SCREEN_WIDTH * SCREEN_HEIGHT;
    static constexpr uint32_t CYCLES_PER_FRAME = 1232 * 228;
    static constexpr size_t SRAM_SIZE = 32 * 1024;
    static constexpr size_t FLASH_SIZE = 64 * 1024;
    static constexpr size_t FLASH1M_SIZE = 128 * 1024;
    static constexpr size_t EEPROM512_SIZE = 512;
    static constexpr size_t EEPROM_SIZE = 8 * 1024;
    static constexpr unsigned AUDIO_SAMPLE_RATE = GbaAudio::SAMPLE_RATE;

    struct FrameResult {
        bool success = false;
        uint32_t cycles = 0;
    };

    explicit GbaSystem(std::vector<uint8_t> rom,
                       std::vector<uint8_t> bios = {},
                       std::shared_ptr<std::vector<uint8_t>> saveData = nullptr,
                       std::shared_ptr<std::vector<uint8_t>> rtcData = nullptr,
                       std::optional<int64_t> rtcSessionEpoch = std::nullopt);
    ~GbaSystem() override;
    GbaSystem(const GbaSystem &) = delete;
    GbaSystem &operator=(const GbaSystem &) = delete;

    void reset();
    FrameResult runFrame();
    // The calling system is the parent and schedules both CPUs.
    void connectLinkChild(GbaSystem &child);
    void disconnectLink();
    using MultiplayerStartSender =
        std::function<void(uint32_t sequence, uint16_t word,
                           uint8_t baudIndex, uint64_t elapsedCycles)>;
    using MultiplayerWordSender =
        std::function<void(uint32_t sequence, uint16_t word)>;
    using MultiplayerReadinessSender = std::function<void(bool ready)>;
    using MultiplayerFrameSender = std::function<void(uint64_t frame)>;
    using RemoteMultiplayerEventPump = std::function<void()>;
    using MultiplayerWordPredictor =
        std::function<std::optional<uint16_t>(uint32_t sequence,
                                              uint16_t localWord)>;
    void configureRemoteMultiplayerLink(bool child, bool connected,
                                        MultiplayerStartSender sendStart,
                                        MultiplayerWordSender sendWord,
                                        MultiplayerReadinessSender sendReady,
                                        MultiplayerFrameSender sendFrame = {},
                                        MultiplayerWordPredictor predictWord = {});
    void setRemoteMultiplayerEventPump(RemoteMultiplayerEventPump pump);
    void setRemoteMultiplayerConnected(bool connected);
    void setRemoteMultiplayerReady(bool ready, uint64_t peerFrame = 0);
    bool receiveRemoteMultiplayerFrame(uint64_t peerFrame);
    uint64_t remoteFrameCount() const { return remoteFrameCount_; }
    uint64_t remoteElapsedCycles() const {
        return emulatedCycles_ - remoteLocalReadyCycle_;
    }
    bool receiveRemoteMultiplayerStart(uint32_t sequence, uint16_t parentWord,
                                       uint8_t baudIndex,
                                       uint64_t elapsedCycles = 0);
    bool receiveRemoteMultiplayerWord(uint32_t sequence, uint16_t childWord);
    void disconnectRemoteMultiplayerLink();
    FrameResult runLinkedFrame();
    void setPressedButtons(uint16_t activeHighMask);

    const std::array<uint16_t, PIXEL_COUNT> &frameBuffer() const {
        return frameBuffer_;
    }
    const Arm7Tdmi &cpu() const { return cpu_; }
    const std::string &fault() const { return cpu_.fault(); }
    uint8_t *getSaveRamData();
    size_t getSaveRamSize() const;
    std::shared_ptr<std::vector<uint8_t>> getSaveRamStorage() const;
    uint8_t *getRtcData();
    size_t getRtcSize() const;
    std::shared_ptr<std::vector<uint8_t>> getRtcStorage() const;
    std::vector<int16_t> takeAudioSamples();

    uint8_t read8(uint32_t address) const override;
    uint16_t read16(uint32_t address) const override;
    uint32_t read32(uint32_t address) const override;
    void write8(uint32_t address, uint8_t value) override;
    void write16(uint32_t address, uint16_t value) override;
    void write32(uint32_t address, uint32_t value) override;
    void registerRamReset(uint8_t flags) override;
    bool hasBios() const override { return !bios_.empty(); }
    uint64_t emulatedCycles() const { return emulatedCycles_; }
    void beginCpuStep() override;
    uint32_t endCpuStep(uint32_t baseCycles) override;

private:
    struct LayerPixel {
        uint16_t color = 0;
        uint16_t order = 0;
        uint8_t priority = 4;
        uint8_t layer = 5;
        bool semiTransparent = false;
        bool valid = false;
    };

    struct PixelLayers {
        LayerPixel top;
        LayerPixel second;
    };

    struct WindowPixel {
        uint8_t layers = 0x3F;
        bool colorEffects = true;
    };

    enum class FlashCommandState {
        Ready,
        UnlockSecond,
        Command,
        EraseUnlockFirst,
        EraseUnlockSecond,
        EraseCommand,
        ProgramData,
        BankData,
    };

    enum class SaveType {
        None,
        Sram,
        Flash64,
        Flash128,
        Eeprom,
    };

    using LayerBuffer = std::array<PixelLayers, SCREEN_WIDTH>;
    using ObjectBuffer = std::array<LayerPixel, SCREEN_WIDTH>;
    using ObjectVisibility = std::array<bool, 128>;
    using ObjectWindowBuffer = std::array<uint8_t, SCREEN_WIDTH>;
    using WindowBuffer = std::array<WindowPixel, SCREEN_WIDTH>;

    struct RemoteMultiplayerStart {
        uint32_t sequence = 0;
        uint16_t parentWord = 0;
        uint8_t baudIndex = 0;
        uint64_t startCycle = 0;
    };

    struct RemoteMultiplayerWord {
        uint32_t sequence = 0;
        uint16_t word = 0;
    };

    static uint32_t videoRamOffset(uint32_t address);
    uint16_t readOamForRendering(uint32_t address) const;
    static SaveType detectSaveType(const std::vector<uint8_t> &rom);
    static bool hasRtcMarker(const std::vector<uint8_t> &rom);
    static bool isEepromAddress(uint32_t address);
    uint8_t read8Impl(uint32_t address) const;
    uint16_t read16Impl(uint32_t address) const;
    void write8Impl(uint32_t address, uint8_t value);
    void write16Impl(uint32_t address, uint16_t value);
    uint16_t loadIo16(uint32_t address) const;
    void storeIo16(uint32_t address, uint16_t value);
    bool inMultiplayerMode() const;
    bool multiplayerReady() const;
    bool multiplayerChild() const;
    void writeSerialControl(uint32_t address, uint16_t value);
    void tickNormalSerial(uint32_t cycles);
    void abortMultiplayerTransfer();
    void beginRemoteMultiplayerTransfer();
    void applyPendingRemoteWord();
    void completeMultiplayerTransfer();
    void tickMultiplayer(uint32_t cycles);
    Arm7Tdmi::StepResult stepCpu();
    void initializeScanlineOam();
    void notifyRemoteReadiness(bool ready);
    void markRemoteLinkActivity();
    bool shouldPaceRemoteFrame();
    void recordCpuBusAccess(uint32_t address, unsigned width) const;
    uint32_t gamePakAccessCycles(unsigned region, bool sequential) const;
    uint8_t readFlash(uint32_t address) const;
    void writeFlash(uint32_t address, uint8_t value);
    uint16_t readEeprom() const;
    void writeEeprom(uint16_t value);
    void finishEepromDma(uint32_t transferCount);
    static void offerPixel(LayerBuffer &pixels, size_t index, uint16_t color,
                           unsigned priority, unsigned layer,
                           unsigned order = 0);
    static void offerObjectPixel(LayerPixel &pixel, uint16_t color,
                                 unsigned priority,
                                 unsigned object, bool semiTransparent);
    void tick(uint32_t cycles);
    void tickTimers(uint32_t cycles);
    void incrementTimer(unsigned timer, uint32_t ticks);
    void updateDisplayStatus();
    void flushCurrentScanline();
    void renderScanline(unsigned y, unsigned firstPixel, unsigned endPixel);
    void renderTextBackground(unsigned background, unsigned y,
                              LayerBuffer &pixels,
                             const WindowBuffer &windows,
                             unsigned firstPixel, unsigned endPixel) const;
    void renderAffineBackground(unsigned background, unsigned y,
                                LayerBuffer &pixels,
                                const WindowBuffer &windows,
                                unsigned firstPixel, unsigned endPixel) const;
    void renderBitmapBackground(unsigned mode, unsigned y, uint16_t display,
                                LayerBuffer &pixels,
                                const WindowBuffer &windows,
                                unsigned firstPixel, unsigned endPixel) const;
    ObjectVisibility selectObjectsForScanline(unsigned y,
                                               uint16_t display) const;
    void renderObjectWindow(unsigned y, bool oneDimensional, bool bitmapMode,
                            const ObjectVisibility &objectVisibility,
                            ObjectWindowBuffer &objectWindow,
                            unsigned firstPixel, unsigned endPixel) const;
    void renderObjects(unsigned y, bool oneDimensional, bool bitmapMode,
                       const ObjectVisibility &objectVisibility,
                       ObjectBuffer &objects,
                       unsigned firstPixel, unsigned endPixel) const;
    void triggerDma(unsigned channel, unsigned timing);
    void requestSoundDma(unsigned fifo);

    std::vector<uint8_t> rom_;
    std::vector<uint8_t> bios_;
    SaveType saveType_ = SaveType::None;
    std::shared_ptr<std::vector<uint8_t>> saveData_;
    GbaCartridgeRtc rtc_;
    FlashCommandState flashCommandState_ = FlashCommandState::Ready;
    uint8_t flashBank_ = 0;
    bool flashIdMode_ = false;
    std::vector<uint8_t> eepromInputBits_;
    uint16_t eepromAddress_ = 0;
    mutable unsigned eepromReadCursor_ = 0;
    mutable bool eepromReadActive_ = false;
    bool eepromDmaWriteActive_ = false;
    std::array<uint8_t, 256 * 1024> externalWorkRam_{};
    std::array<uint8_t, 32 * 1024> internalWorkRam_{};
    std::array<uint8_t, 0x400> io_{};
    GbaSystem *linkPeer_ = nullptr;
    bool linkChild_ = false;
    bool remoteLinkEnabled_ = false;
    bool remoteLinkChild_ = false;
    bool remoteLinkConnected_ = false;
    bool remotePeerReady_ = false;
    bool remotePeerWordReady_ = false;
    bool remoteReplySent_ = false;
    std::deque<RemoteMultiplayerStart> remotePendingStarts_;
    std::deque<RemoteMultiplayerWord> remotePendingWords_;
    uint32_t remoteTransferSequence_ = 0;
    uint32_t remoteActiveSequence_ = 0;
    MultiplayerStartSender sendRemoteStart_;
    MultiplayerWordSender sendRemoteWord_;
    MultiplayerReadinessSender sendRemoteReady_;
    MultiplayerFrameSender sendRemoteFrame_;
    RemoteMultiplayerEventPump remoteEventPump_;
    MultiplayerWordPredictor predictRemoteWord_;
    std::unordered_map<uint32_t, uint16_t> remotePredictedWords_;
    bool multiplayerBusy_ = false;
    bool multiplayerError_ = false;
    uint32_t multiplayerCycles_ = 0;
    uint32_t remoteFrameCycles_ = 0;
    uint64_t remoteFrameCount_ = 0;
    uint64_t remoteLocalReadyCycle_ = 0;
    uint64_t remotePeerCycleOffset_ = 0;
    uint64_t remotePeerFrameCount_ = 0;
    uint64_t remoteLocalReadyFrame_ = 0;
    uint64_t remotePeerReadyFrame_ = 0;
    uint64_t remoteHotLocalBase_ = 0;
    uint64_t remoteHotPeerBase_ = 0;
    uint64_t remoteLastActivityFrame_ = 0;
    bool remotePeerFrameKnown_ = false;
    bool remoteLocalReady_ = false;
    bool remoteLinkHot_ = false;
    bool remotePredictionUsed_ = false;
    uint64_t emulatedCycles_ = 0;
    bool normalSerialBusy_ = false;
    uint32_t normalSerialCycles_ = 0;
    uint32_t linkedFrameCarry_ = 0;
    std::array<uint16_t, 2> multiplayerWords_{};
    std::array<uint8_t, 1024> paletteRam_{};
    std::array<uint8_t, 96 * 1024> videoRam_{};
    std::array<uint8_t, 1024> oam_{};
    std::array<uint8_t, 1024> renderedOam_{};
    std::array<uint32_t, 4> dmaSource_{};
    std::array<uint32_t, 4> dmaDestination_{};
    std::array<bool, 4> dmaInitialized_{};
    bool dmaActive_ = false;
    std::array<uint16_t, 4> timerReload_{};
    std::array<uint16_t, 4> timerCounter_{};
    std::array<uint32_t, 4> timerPrescalerCycles_{};
    mutable bool cpuStepActive_ = false;
    mutable bool cpuStepFirstBusAccess_ = true;
    mutable uint32_t cpuStepWaitCycles_ = 0;
    mutable bool lastGamePakAccessValid_ = false;
    mutable uint32_t lastGamePakAccessAddress_ = 0;
    mutable unsigned lastGamePakAccessWidth_ = 0;
    mutable unsigned lastGamePakAccessRegion_ = 0;
    mutable GbaAudio audio_;
    std::array<uint16_t, PIXEL_COUNT> frameBuffer_{};
    Arm7Tdmi cpu_;
    uint16_t pressedButtons_ = 0;
    uint16_t remoteInputLatch_ = 0;
    bool remoteFrameStalled_ = false;
    uint32_t frameCycle_ = 0;
    bool renderedOamInitialized_ = false;
    uint16_t renderedScanline_ = 0xFFFF;
    unsigned renderedPixel_ = 0;
};

} // namespace LOTUSGBA

#endif // LOTUSGBA_GBA_SYSTEM_HPP
