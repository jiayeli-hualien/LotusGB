#include "gba_system.hpp"

#include <algorithm>
#include <stdexcept>

namespace LOTUSGBA {
namespace {

constexpr uint32_t REG_DISPCNT = 0x04000000;
constexpr uint32_t REG_DISPSTAT = 0x04000004;
constexpr uint32_t REG_VCOUNT = 0x04000006;
constexpr uint32_t REG_KEYINPUT = 0x04000130;
constexpr uint32_t VRAM_BASE = 0x06000000;

} // namespace

GbaSystem::GbaSystem(std::vector<uint8_t> rom) : rom_(std::move(rom)) {
    if (rom_.size() < 0xC4)
        throw std::invalid_argument("GBA ROM must contain a cartridge entry point");
    reset();
}

void GbaSystem::reset() {
    externalWorkRam_.fill(0);
    internalWorkRam_.fill(0);
    io_.fill(0);
    paletteRam_.fill(0);
    videoRam_.fill(0);
    oam_.fill(0);
    frameBuffer_.fill(0x7FFF);
    pressedButtons_ = 0;
    frameCycle_ = 0;
    cpu_.reset(0x08000000, 0x03007F00);
    updateDisplayStatus();
}

void GbaSystem::setPressedButtons(uint16_t activeHighMask) {
    pressedButtons_ = activeHighMask & 0x03FF;
}

uint32_t GbaSystem::videoRamOffset(uint32_t address) {
    uint32_t offset = address & 0x1FFFF;
    if (offset >= 0x18000)
        offset = 0x10000 + (offset - 0x18000);
    return offset;
}

uint8_t GbaSystem::read8(uint32_t address) const {
    if (address >= 0x02000000 && address < 0x03000000)
        return externalWorkRam_[(address - 0x02000000) & 0x3FFFF];
    if (address >= 0x03000000 && address < 0x04000000)
        return internalWorkRam_[(address - 0x03000000) & 0x7FFF];
    if (address >= 0x04000000 && address < 0x04000400) {
        if (address == REG_KEYINPUT || address == REG_KEYINPUT + 1) {
            const uint16_t value = static_cast<uint16_t>(~pressedButtons_) & 0x03FF;
            return static_cast<uint8_t>(value >> ((address - REG_KEYINPUT) * 8));
        }
        return io_[address - 0x04000000];
    }
    if (address >= 0x05000000 && address < 0x06000000)
        return paletteRam_[(address - 0x05000000) & 0x3FF];
    if (address >= 0x06000000 && address < 0x07000000)
        return videoRam_[videoRamOffset(address)];
    if (address >= 0x07000000 && address < 0x08000000)
        return oam_[(address - 0x07000000) & 0x3FF];
    if (address >= 0x08000000 && address < 0x0E000000 && !rom_.empty()) {
        const size_t offset = (address & 0x01FFFFFF) % rom_.size();
        return rom_[offset];
    }
    return 0;
}

uint16_t GbaSystem::read16(uint32_t address) const {
    address &= ~uint32_t{1};
    return static_cast<uint16_t>(read8(address) |
                                 (static_cast<uint16_t>(read8(address + 1)) << 8));
}

uint32_t GbaSystem::read32(uint32_t address) const {
    address &= ~uint32_t{3};
    return static_cast<uint32_t>(read16(address)) |
           (static_cast<uint32_t>(read16(address + 2)) << 16);
}

void GbaSystem::write8(uint32_t address, uint8_t value) {
    if (address >= 0x02000000 && address < 0x03000000) {
        externalWorkRam_[(address - 0x02000000) & 0x3FFFF] = value;
        return;
    }
    if (address >= 0x03000000 && address < 0x04000000) {
        internalWorkRam_[(address - 0x03000000) & 0x7FFF] = value;
        return;
    }
    if (address >= 0x04000000 && address < 0x04000400) {
        if (address == REG_KEYINPUT || address == REG_KEYINPUT + 1)
            return;
        io_[address - 0x04000000] = value;
        return;
    }
    if (address >= 0x05000000 && address < 0x06000000) {
        paletteRam_[(address - 0x05000000) & 0x3FF] = value;
        return;
    }
    if (address >= 0x06000000 && address < 0x07000000) {
        videoRam_[videoRamOffset(address)] = value;
        return;
    }
    if (address >= 0x07000000 && address < 0x08000000)
        oam_[(address - 0x07000000) & 0x3FF] = value;
}

void GbaSystem::write16(uint32_t address, uint16_t value) {
    address &= ~uint32_t{1};
    write8(address, static_cast<uint8_t>(value));
    write8(address + 1, static_cast<uint8_t>(value >> 8));
}

void GbaSystem::write32(uint32_t address, uint32_t value) {
    address &= ~uint32_t{3};
    write16(address, static_cast<uint16_t>(value));
    write16(address + 2, static_cast<uint16_t>(value >> 16));
}

void GbaSystem::tick(uint32_t cycles) {
    frameCycle_ = (frameCycle_ + cycles) % CYCLES_PER_FRAME;
    updateDisplayStatus();
}

void GbaSystem::updateDisplayStatus() {
    const uint16_t line = static_cast<uint16_t>(frameCycle_ / 1232);
    io_[REG_VCOUNT - 0x04000000] = static_cast<uint8_t>(line);
    io_[REG_VCOUNT - 0x04000000 + 1] = static_cast<uint8_t>(line >> 8);
    uint16_t status = static_cast<uint16_t>(io_[REG_DISPSTAT - 0x04000000] |
        (static_cast<uint16_t>(io_[REG_DISPSTAT - 0x04000000 + 1]) << 8));
    status = static_cast<uint16_t>((status & ~0x0007) | (line >= 160 ? 1 : 0));
    io_[REG_DISPSTAT - 0x04000000] = static_cast<uint8_t>(status);
    io_[REG_DISPSTAT - 0x04000000 + 1] = static_cast<uint8_t>(status >> 8);
}

void GbaSystem::renderFrame() {
    const uint16_t display = read16(REG_DISPCNT);
    const unsigned mode = display & 7;
    if ((display & (1u << 7)) != 0) {
        frameBuffer_.fill(0x7FFF);
        return;
    }
    if (mode == 3 && (display & (1u << 10)) != 0) {
        for (size_t pixel = 0; pixel < PIXEL_COUNT; ++pixel)
            frameBuffer_[pixel] = read16(VRAM_BASE + static_cast<uint32_t>(pixel * 2));
        return;
    }
    if (mode == 4 && (display & (1u << 10)) != 0) {
        const uint32_t page = (display & (1u << 4)) != 0 ? 0xA000 : 0;
        for (size_t pixel = 0; pixel < PIXEL_COUNT; ++pixel) {
            const unsigned paletteIndex =
                videoRam_[videoRamOffset(VRAM_BASE + page +
                    static_cast<uint32_t>(pixel))];
            frameBuffer_[pixel] = static_cast<uint16_t>(
                read16(0x05000000 + paletteIndex * 2) & 0x7FFF);
        }
        return;
    }

    const uint16_t backdrop = read16(0x05000000);
    frameBuffer_.fill(backdrop & 0x7FFF);
}

GbaSystem::FrameResult GbaSystem::runFrame() {
    uint32_t elapsed = 0;
    while (elapsed < CYCLES_PER_FRAME) {
        const Arm7Tdmi::StepResult result = cpu_.step(*this);
        if (!result.success)
            return {false, elapsed};
        elapsed += result.cycles;
        tick(result.cycles);
    }
    renderFrame();
    return {true, elapsed};
}

} // namespace LOTUSGBA
