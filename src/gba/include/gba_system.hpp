#ifndef LOTUSGBA_GBA_SYSTEM_HPP
#define LOTUSGBA_GBA_SYSTEM_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "arm7tdmi.hpp"

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

    struct FrameResult {
        bool success = false;
        uint32_t cycles = 0;
    };

    explicit GbaSystem(std::vector<uint8_t> rom,
                       std::vector<uint8_t> bios = {},
                       std::shared_ptr<std::vector<uint8_t>> saveData = nullptr);

    void reset();
    FrameResult runFrame();
    void setPressedButtons(uint16_t activeHighMask);

    const std::array<uint16_t, PIXEL_COUNT> &frameBuffer() const {
        return frameBuffer_;
    }
    const Arm7Tdmi &cpu() const { return cpu_; }
    const std::string &fault() const { return cpu_.fault(); }
    uint8_t *getSaveRamData();
    size_t getSaveRamSize() const;
    std::shared_ptr<std::vector<uint8_t>> getSaveRamStorage() const;

    uint8_t read8(uint32_t address) const override;
    uint16_t read16(uint32_t address) const override;
    uint32_t read32(uint32_t address) const override;
    void write8(uint32_t address, uint8_t value) override;
    void write16(uint32_t address, uint16_t value) override;
    void write32(uint32_t address, uint32_t value) override;
    void registerRamReset(uint8_t flags) override;
    bool hasBios() const override { return !bios_.empty(); }

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

    static uint32_t videoRamOffset(uint32_t address);
    uint16_t readOamForRendering(uint32_t address) const;
    static SaveType detectSaveType(const std::vector<uint8_t> &rom);
    static bool isEepromAddress(uint32_t address);
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

    std::vector<uint8_t> rom_;
    std::vector<uint8_t> bios_;
    SaveType saveType_ = SaveType::None;
    std::shared_ptr<std::vector<uint8_t>> saveData_;
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
    std::array<uint16_t, PIXEL_COUNT> frameBuffer_{};
    Arm7Tdmi cpu_;
    uint16_t pressedButtons_ = 0;
    uint32_t frameCycle_ = 0;
    bool renderedOamInitialized_ = false;
    uint16_t renderedScanline_ = 0xFFFF;
    unsigned renderedPixel_ = 0;
};

} // namespace LOTUSGBA

#endif // LOTUSGBA_GBA_SYSTEM_HPP
