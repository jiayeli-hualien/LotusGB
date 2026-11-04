#ifndef LOTUSGBA_GBA_SYSTEM_HPP
#define LOTUSGBA_GBA_SYSTEM_HPP

#include <array>
#include <cstddef>
#include <cstdint>
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

    struct FrameResult {
        bool success = false;
        uint32_t cycles = 0;
    };

    explicit GbaSystem(std::vector<uint8_t> rom);

    void reset();
    FrameResult runFrame();
    void setPressedButtons(uint16_t activeHighMask);

    const std::array<uint16_t, PIXEL_COUNT> &frameBuffer() const {
        return frameBuffer_;
    }
    const Arm7Tdmi &cpu() const { return cpu_; }
    const std::string &fault() const { return cpu_.fault(); }

    uint8_t read8(uint32_t address) const override;
    uint16_t read16(uint32_t address) const override;
    uint32_t read32(uint32_t address) const override;
    void write8(uint32_t address, uint8_t value) override;
    void write16(uint32_t address, uint16_t value) override;
    void write32(uint32_t address, uint32_t value) override;

private:
    static uint32_t videoRamOffset(uint32_t address);
    void tick(uint32_t cycles);
    void updateDisplayStatus();
    void renderFrame();

    std::vector<uint8_t> rom_;
    std::array<uint8_t, 256 * 1024> externalWorkRam_{};
    std::array<uint8_t, 32 * 1024> internalWorkRam_{};
    std::array<uint8_t, 0x400> io_{};
    std::array<uint8_t, 1024> paletteRam_{};
    std::array<uint8_t, 96 * 1024> videoRam_{};
    std::array<uint8_t, 1024> oam_{};
    std::array<uint16_t, PIXEL_COUNT> frameBuffer_{};
    Arm7Tdmi cpu_;
    uint16_t pressedButtons_ = 0;
    uint32_t frameCycle_ = 0;
};

} // namespace LOTUSGBA

#endif // LOTUSGBA_GBA_SYSTEM_HPP
