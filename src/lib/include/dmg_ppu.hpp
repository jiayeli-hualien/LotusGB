#ifndef LOTUSGB_DMG_PPU_HPP
#define LOTUSGB_DMG_PPU_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "IInterruptController.hpp"
#include "address_bus.hpp"
#include "system_clock.hpp"

namespace LOTUSGB {

// Initial DMG PPU timing, memory interface, and scanline renderer. Pixels are
// emitted during mode 3, with SCX fine-scroll, visible-window, and object-fetch
// stalls included.
class DmgPpu final : public IAddressRegion, public IClockedComponent {
public:
    static constexpr size_t FRAME_WIDTH = 160;
    static constexpr size_t FRAME_HEIGHT = 144;
    static constexpr size_t OAM_SIZE = 0x00A0;
    using FrameBuffer = std::array<uint8_t, FRAME_WIDTH * FRAME_HEIGHT>;

    explicit DmgPpu(IInterruptController &interrupts);

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void advanceTCycles(uint64_t tCycles) override;
    void initializeDmgPostBootState();
    void writeOamFromDma(size_t offset, uint8_t value);

    // Each pixel is a DMG shade index (0 is lightest, 3 is darkest), after
    // applying BGP/OBP0/OBP1. The frontend chooses the actual output colors.
    const FrameBuffer &getFrameBuffer() const;

private:
    static constexpr uint16_t MODE3_BASE_DOTS = 172;
    static constexpr size_t MAX_OBJECTS_PER_SCANLINE = 10;

    struct BackgroundFetcherState {
        enum class Phase : uint8_t {
            TileMapAddress,
            TileMapData,
            TileDataLowAddress,
            TileDataLow,
            TileDataHighAddress,
            TileDataHigh,
            Push,
            AdvanceTileColumn
        };

        uint8_t tile = 0;
        uint8_t low = 0;
        uint8_t high = 0;
        uint16_t tileMapAddress = 0;
        uint16_t lowPlaneAddress = 0;
        uint16_t highPlaneAddress = 0;
        uint16_t tileColumn = 0;
        uint8_t startupDotsRemaining = 6;
        Phase phase = Phase::TileMapAddress;
    };

    struct BackgroundFifo {
        static constexpr size_t CAPACITY = 16;

        std::array<uint8_t, CAPACITY> pixels{};
        uint8_t readIndex = 0;
        uint8_t size = 0;

        void clear() {
            readIndex = 0;
            size = 0;
        }

        bool canPushRow() const { return size == 0; }

        bool pushRow(uint8_t low, uint8_t high) {
            if (!canPushRow())
                return false;

            for (uint8_t pixel = 0; pixel < 8; ++pixel) {
                const uint8_t bit = static_cast<uint8_t>(7 - pixel);
                const uint8_t color = static_cast<uint8_t>(
                    ((low >> bit) & 0x01) | (((high >> bit) & 0x01) << 1));
                pixels[(readIndex + size) % CAPACITY] = color;
                ++size;
            }
            return true;
        }

        bool pop(uint8_t &pixel) {
            if (size == 0)
                return false;
            pixel = pixels[readIndex];
            readIndex = static_cast<uint8_t>((readIndex + 1) % CAPACITY);
            --size;
            return true;
        }
    };

    struct ObjectPixel {
        uint8_t colorIndex = 0;
        bool usesPalette1 = false;
        bool behindBackground = false;
    };

    struct ObjectFetchEvent {
        std::array<ObjectPixel, 8> pixels{};
        uint8_t tileIndex = 0;
        uint8_t tileRow = 0;
        uint8_t attributes = 0;
        uint8_t low = 0;
        uint8_t high = 0;
        uint8_t startX = 0;
        uint8_t firstPixelIndex = 0;
        uint8_t alignmentDots = 0;
        uint8_t transferDots = 0;
    };

    struct ObjectFifo {
        static constexpr size_t CAPACITY = 16;

        std::array<ObjectPixel, CAPACITY> pixels{};
        uint8_t readIndex = 0;

        void reset() {
            pixels.fill({});
            readIndex = 0;
        }

        void overlay(const ObjectFetchEvent &event) {
            if (event.firstPixelIndex >= event.pixels.size())
                return;

            for (size_t source = event.firstPixelIndex;
                 source < event.pixels.size(); ++source) {
                const size_t fifoOffset = source - event.firstPixelIndex;
                const size_t index = (readIndex + fifoOffset) % CAPACITY;
                const ObjectPixel &incoming = event.pixels[source];
                if (incoming.colorIndex != 0 && pixels[index].colorIndex == 0)
                    pixels[index] = incoming;
            }
        }

        void popAndShift(ObjectPixel &pixel) {
            pixel = pixels[readIndex];
            pixels[readIndex] = {};
            readIndex = static_cast<uint8_t>((readIndex + 1) % CAPACITY);
        }
    };

    // These coarse stages preserve the current penalty schedule and expose
    // separate plane-read points. Their dot alignment is not hardware-validated.
    struct ObjectFetcherState {
        enum class FetchAction : uint8_t { None, ReadLowPlane, ReadHighPlane };
        enum class Phase : uint8_t {
            Idle,
            AlignmentWait,
            FetchSetup,
            DataTransfer
        };

        Phase phase = Phase::Idle;
        uint8_t alignmentDotsRemaining = 0;
        uint8_t fetchSetupDotsRemaining = 0;
        uint8_t dataTransferDotsRemaining = 0;

        bool active() const { return phase != Phase::Idle; }

        void reset() {
            phase = Phase::Idle;
            alignmentDotsRemaining = 0;
            fetchSetupDotsRemaining = 0;
            dataTransferDotsRemaining = 0;
        }

        void begin(const ObjectFetchEvent &event) {
            // Keep the four-dot setup and remaining transfer time separate.
            // Low/high plane reads are attached to their respective boundaries;
            // DmgPpu coordinates BG fetch catch-up around this coarse schedule.
            fetchSetupDotsRemaining = 4;
            dataTransferDotsRemaining =
                static_cast<uint8_t>(event.transferDots - fetchSetupDotsRemaining);
            alignmentDotsRemaining = event.alignmentDots;
            if (event.alignmentDots > 0) {
                phase = Phase::AlignmentWait;
            } else {
                phase = Phase::FetchSetup;
            }
        }

        FetchAction advanceOneDot() {
            switch (phase) {
            case Phase::Idle:
                return FetchAction::None;
            case Phase::AlignmentWait:
                if (--alignmentDotsRemaining == 0)
                    phase = Phase::FetchSetup;
                return FetchAction::None;
            case Phase::FetchSetup:
                if (--fetchSetupDotsRemaining == 0) {
                    phase = Phase::DataTransfer;
                    return FetchAction::ReadLowPlane;
                }
                return FetchAction::None;
            case Phase::DataTransfer:
                if (--dataTransferDotsRemaining == 0) {
                    phase = Phase::Idle;
                    return FetchAction::ReadHighPlane;
                }
                return FetchAction::None;
            }
            return FetchAction::None;
        }
    };

    struct Mode3State {
        std::array<uint8_t, FRAME_WIDTH> windowColorIndices{};
        std::array<ObjectFetchEvent, MAX_OBJECTS_PER_SCANLINE> objectFetchEvents{};

        BackgroundFifo backgroundFifo{};
        ObjectFifo objectFifo{};
        BackgroundFetcherState fetcher{};
        ObjectFetcherState objectFetcher{};
        uint16_t duration = MODE3_BASE_DOTS;
        uint8_t fineScroll = 0;
        uint8_t outputX = 0;
        uint8_t windowStartX = 0;
        uint8_t windowPenaltyDotsRemaining = 0;
        uint8_t objectFetchEventCount = 0;
        uint8_t nextObjectFetchEvent = 0;
        uint8_t activeObjectFetchEvent = 0;
        bool windowPenaltyPending = false;
        bool windowStarted = false;
        bool drewWindow = false;

        void reset() {
            windowColorIndices.fill(0);
            objectFetchEvents.fill({});
            backgroundFifo.clear();
            objectFifo.reset();
            fetcher = {};
            objectFetcher.reset();
            duration = MODE3_BASE_DOTS;
            fineScroll = 0;
            outputX = 0;
            windowStartX = 0;
            windowPenaltyDotsRemaining = 0;
            objectFetchEventCount = 0;
            nextObjectFetchEvent = 0;
            activeObjectFetchEvent = 0;
            windowPenaltyPending = false;
            windowStarted = false;
            drewWindow = false;
        }
    };

    uint8_t getMode() const;
    uint8_t reportedLy() const;
    bool lcdEnabled() const;
    bool lyEqualsLyc() const;
    bool statInterruptSignal() const;
    void updateStatInterruptLine();
    void advanceDot();
    void advanceMode3Pipeline();
    void advanceBackgroundFetcherForObjectFetch();
    void advanceActiveObjectFetch();
    void emitMode3Pixel();
    void prepareScanline();
    void renderPixel(size_t screenX, uint8_t backgroundColorIndex,
                     const ObjectPixel &objectPixel);
    void advanceBackgroundFetch();
    uint16_t backgroundBitplaneAddressOffset(bool high) const;
    void prepareObjects(bool includeFetchPenalties);
    void readObjectTilePlane(ObjectFetchEvent &event, bool high) const;
    void decodeObjectTileRow(ObjectFetchEvent &event) const;
    uint8_t getTilePixel(uint8_t tileIndex, uint8_t x, uint8_t y,
                         bool unsignedTileData) const;
    uint8_t getTileMapPixel(uint16_t tileMapOffset, uint8_t x, uint8_t y) const;
    static uint8_t applyPalette(uint8_t palette, uint8_t colorIndex);

    IInterruptController &interrupts;
    std::array<uint8_t, 0x2000> videoRam{};
    std::array<uint8_t, OAM_SIZE> objectRam{};
    FrameBuffer frameBuffer{};
    Mode3State mode3{};

    uint8_t lcdControl = 0;
    uint8_t statInterruptEnable = 0;
    uint8_t scrollY = 0;
    uint8_t scrollX = 0;
    uint8_t ly = 0;
    uint8_t lyCompare = 0;
    uint8_t bgPalette = 0;
    uint8_t objectPalette0 = 0;
    uint8_t objectPalette1 = 0;
    uint8_t windowY = 0;
    uint8_t windowX = 0;
    uint8_t windowLineCounter = 0;
    bool windowYTriggered = false;
    uint16_t dot = 0;
    bool startupScanline = false;
    bool lycCoincidence = true;
    bool statInterruptLine = false;
};

}

#endif // LOTUSGB_DMG_PPU_HPP
