#include "include/dmg_ppu.hpp"

namespace LOTUSGB {

namespace {
constexpr uint16_t VRAM_START = 0x8000;
constexpr uint16_t VRAM_END = 0x9FFF;
constexpr uint16_t OAM_START = 0xFE00;
constexpr uint16_t OAM_END = 0xFE9F;

constexpr uint16_t LCDC_ADDRESS = 0xFF40;
constexpr uint16_t STAT_ADDRESS = 0xFF41;
constexpr uint16_t SCY_ADDRESS = 0xFF42;
constexpr uint16_t SCX_ADDRESS = 0xFF43;
constexpr uint16_t LY_ADDRESS = 0xFF44;
constexpr uint16_t LYC_ADDRESS = 0xFF45;
constexpr uint16_t BGP_ADDRESS = 0xFF47;
constexpr uint16_t OBP0_ADDRESS = 0xFF48;
constexpr uint16_t OBP1_ADDRESS = 0xFF49;
constexpr uint16_t WY_ADDRESS = 0xFF4A;
constexpr uint16_t WX_ADDRESS = 0xFF4B;

constexpr uint8_t LCD_ENABLE_MASK = 0x80;
constexpr uint8_t STAT_LYC_INTERRUPT_MASK = 0x40;
constexpr uint8_t STAT_MODE2_INTERRUPT_MASK = 0x20;
constexpr uint8_t STAT_MODE1_INTERRUPT_MASK = 0x10;
constexpr uint8_t STAT_MODE0_INTERRUPT_MASK = 0x08;
constexpr uint8_t STAT_INTERRUPT_ENABLE_MASK = 0x78;
constexpr uint8_t STAT_COINCIDENCE_MASK = 0x04;

constexpr uint16_t DOTS_PER_SCANLINE = 456;
constexpr uint16_t MODE2_DOTS = 80;
constexpr uint16_t MODE3_DOTS = 172;
constexpr uint8_t VISIBLE_SCANLINES = 144;
constexpr uint8_t SCANLINES_PER_FRAME = 154;
}

DmgPpu::DmgPpu(IInterruptController &interrupts) : interrupts(interrupts) {}

uint8_t DmgPpu::read(uint16_t address) {
    if (address >= VRAM_START && address <= VRAM_END) {
        if (lcdEnabled() && getMode() == 3)
            return 0xFF;
        return videoRam[address - VRAM_START];
    }

    if (address >= OAM_START && address <= OAM_END) {
        const uint8_t mode = getMode();
        if (lcdEnabled() && (mode == 2 || mode == 3))
            return 0xFF;
        return objectRam[address - OAM_START];
    }

    switch (address) {
        case LCDC_ADDRESS:
            return lcdControl;
        case STAT_ADDRESS: {
            const uint8_t coincidence = lyEqualsLyc() ? STAT_COINCIDENCE_MASK : 0;
            return static_cast<uint8_t>(0x80 | statInterruptEnable | coincidence | getMode());
        }
        case SCY_ADDRESS:
            return scrollY;
        case SCX_ADDRESS:
            return scrollX;
        case LY_ADDRESS:
            return ly;
        case LYC_ADDRESS:
            return lyCompare;
        case BGP_ADDRESS:
            return bgPalette;
        case OBP0_ADDRESS:
            return objectPalette0;
        case OBP1_ADDRESS:
            return objectPalette1;
        case WY_ADDRESS:
            return windowY;
        case WX_ADDRESS:
            return windowX;
        default:
            return 0xFF;
    }
}

void DmgPpu::write(uint16_t address, uint8_t value) {
    if (address >= VRAM_START && address <= VRAM_END) {
        if (!lcdEnabled() || getMode() != 3)
            videoRam[address - VRAM_START] = value;
        return;
    }

    if (address >= OAM_START && address <= OAM_END) {
        const uint8_t mode = getMode();
        if (!lcdEnabled() || (mode != 2 && mode != 3))
            objectRam[address - OAM_START] = value;
        return;
    }

    switch (address) {
        case LCDC_ADDRESS: {
            const bool wasEnabled = lcdEnabled();
            lcdControl = value;
            if (wasEnabled != lcdEnabled()) {
                dot = 0;
                ly = 0;
                updateStatInterruptLine();
            }
            break;
        }
        case STAT_ADDRESS:
            statInterruptEnable = value & STAT_INTERRUPT_ENABLE_MASK;
            updateStatInterruptLine();
            break;
        case SCY_ADDRESS:
            scrollY = value;
            break;
        case SCX_ADDRESS:
            scrollX = value;
            break;
        case LY_ADDRESS:
            // LY is read-only.
            break;
        case LYC_ADDRESS:
            lyCompare = value;
            updateStatInterruptLine();
            break;
        case BGP_ADDRESS:
            bgPalette = value;
            break;
        case OBP0_ADDRESS:
            objectPalette0 = value;
            break;
        case OBP1_ADDRESS:
            objectPalette1 = value;
            break;
        case WY_ADDRESS:
            windowY = value;
            break;
        case WX_ADDRESS:
            windowX = value;
            break;
        default:
            break;
    }
}

void DmgPpu::advanceTCycles(uint64_t tCycles) {
    if (!lcdEnabled())
        return;

    for (uint64_t cycle = 0; cycle < tCycles; ++cycle)
        advanceDot();
}

uint8_t DmgPpu::getMode() const {
    if (!lcdEnabled())
        return 0;
    if (ly >= VISIBLE_SCANLINES)
        return 1;
    if (dot < MODE2_DOTS)
        return 2;
    if (dot < MODE2_DOTS + MODE3_DOTS)
        return 3;
    return 0;
}

bool DmgPpu::lcdEnabled() const {
    return (lcdControl & LCD_ENABLE_MASK) != 0;
}

bool DmgPpu::lyEqualsLyc() const {
    return ly == lyCompare;
}

bool DmgPpu::statInterruptSignal() const {
    if (!lcdEnabled())
        return false;

    const uint8_t mode = getMode();
    return ((mode == 0) && (statInterruptEnable & STAT_MODE0_INTERRUPT_MASK)) ||
           ((mode == 1) && (statInterruptEnable & STAT_MODE1_INTERRUPT_MASK)) ||
           ((mode == 2) && (statInterruptEnable & STAT_MODE2_INTERRUPT_MASK)) ||
           (lyEqualsLyc() && (statInterruptEnable & STAT_LYC_INTERRUPT_MASK));
}

void DmgPpu::updateStatInterruptLine() {
    const bool signal = statInterruptSignal();
    if (signal && !statInterruptLine)
        interrupts.requestInterrupt(INTERRUPT_LCD_STAT);
    statInterruptLine = signal;
}

void DmgPpu::advanceDot() {
    ++dot;
    if (dot == DOTS_PER_SCANLINE) {
        dot = 0;
        ++ly;
        if (ly == VISIBLE_SCANLINES)
            interrupts.requestInterrupt(INTERRUPT_VBLANK);
        else if (ly == SCANLINES_PER_FRAME)
            ly = 0;
    }
    updateStatInterruptLine();
}

}
