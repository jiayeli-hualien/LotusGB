#include "gba_system.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace LOTUSGBA {
namespace {

constexpr uint32_t REG_DISPCNT = 0x04000000;
constexpr uint32_t REG_DISPSTAT = 0x04000004;
constexpr uint32_t REG_VCOUNT = 0x04000006;
constexpr uint32_t REG_POSTFLG = 0x04000300;
constexpr uint32_t REG_KEYINPUT = 0x04000130;
constexpr uint32_t REG_IE = 0x04000200;
constexpr uint32_t REG_IF = 0x04000202;
constexpr uint32_t REG_WAITCNT = 0x04000204;
constexpr uint32_t REG_IME = 0x04000208;
constexpr uint32_t TIMER_BASE = 0x04000100;
constexpr uint32_t REG_SIOMULTI0 = 0x04000120;
constexpr uint32_t REG_SIOCNT = 0x04000128;
constexpr uint32_t REG_SIODATA8 = 0x0400012A;
constexpr uint32_t REG_SIOMLT_SEND = 0x0400012A;
constexpr uint32_t REG_RCNT = 0x04000134;
constexpr uint16_t SIO_START = 0x0080;
constexpr uint16_t SIO_IRQ_ENABLE = 0x4000;
constexpr uint16_t SIO_MULTI_WRITABLE = 0x6F03;
constexpr uint16_t IRQ_SERIAL = 1u << 7;
constexpr uint32_t MULTIPLAYER_BAUD[] = {9600, 38400, 57600, 115200};
constexpr uint32_t NORMAL_BAUD[] = {256'000, 2'000'000};
constexpr uint32_t CPU_CLOCK = 16'777'216;
// Network-mode compatibility policy: render/frontend time remains free-running,
// while guest progress is loosely bounded only during active multiplayer SIO.
// This is not a model of a cable synchronizing console frame clocks; it preserves
// actual SIO words and completion behavior while limiting guest-visible drift.
constexpr uint64_t REMOTE_LINK_MAX_FRAME_LEAD = 2;
constexpr uint64_t REMOTE_LINK_HOT_WINDOW_FRAMES = 8;
constexpr size_t REMOTE_LINK_MAX_QUEUED_STARTS = 64;
constexpr size_t REMOTE_LINK_MAX_QUEUED_WORDS = 64;
constexpr uint32_t CYCLES_PER_SCANLINE = 1232;
constexpr uint32_t HDRAW_CYCLES = 960;
constexpr uint16_t TOTAL_SCANLINES = 228;
constexpr uint16_t IRQ_VBLANK = 1u << 0;
constexpr uint16_t IRQ_HBLANK = 1u << 1;
constexpr uint16_t IRQ_VCOUNT = 1u << 2;
constexpr uint32_t VRAM_BASE = 0x06000000;
constexpr uint32_t PALETTE_BASE = 0x05000000;
constexpr uint32_t OBJ_PALETTE_BASE = PALETTE_BASE + 0x200;
constexpr uint32_t OAM_BASE = 0x07000000;
constexpr uint32_t FLASH_BASE = 0x0E000000;
constexpr uint32_t FLASH_WINDOW_SIZE = 0x10000;
constexpr uint32_t FLASH_BANK_SIZE = 0x10000;
constexpr uint32_t FLASH_SECTOR_SIZE = 0x1000;
constexpr uint32_t EEPROM_BASE = 0x0D000000;
constexpr uint32_t EEPROM_WINDOW_SIZE = 0x01000000;

int64_t fixedToInteger(int64_t value) {
    return value >= 0 ? value / 256 : -((-value + 255) / 256);
}

int32_t signExtend28(uint32_t value) {
    value &= 0x0FFFFFFF;
    if ((value & 0x08000000) != 0)
        value |= 0xF0000000;
    return static_cast<int32_t>(value);
}

bool decodeObjectBaseTile(uint16_t attribute2, bool eightBit,
                          bool bitmapMode, unsigned &baseTile) {
    baseTile = attribute2 & 0x03FF;
    if (bitmapMode) {
        if (baseTile < 512)
            return false;
        baseTile -= 512;
    }
    if (eightBit)
        baseTile &= ~1u;
    return true;
}

constexpr unsigned OBJ_DIMENSIONS[3][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
};

} // namespace

GbaSystem::GbaSystem(
    std::vector<uint8_t> rom, std::vector<uint8_t> bios,
    std::shared_ptr<std::vector<uint8_t>> saveData,
    std::shared_ptr<std::vector<uint8_t>> rtcData,
    std::optional<int64_t> rtcSessionEpoch)
    : rom_(std::move(rom)), bios_(std::move(bios)),
      saveType_(detectSaveType(rom_)), saveData_(std::move(saveData)),
      rtc_(hasRtcMarker(rom_), std::move(rtcData), rtcSessionEpoch) {
    if (rom_.size() < 0xC4)
        throw std::invalid_argument("GBA ROM must contain a cartridge entry point");
    if (!bios_.empty() && bios_.size() != 0x4000)
        throw std::invalid_argument("GBA BIOS must contain exactly 16 KiB");
    size_t expectedSize = 0;
    switch (saveType_) {
    case SaveType::None: break;
    case SaveType::Sram: expectedSize = SRAM_SIZE; break;
    case SaveType::Flash64: expectedSize = FLASH_SIZE; break;
    case SaveType::Flash128: expectedSize = FLASH1M_SIZE; break;
    case SaveType::Eeprom: expectedSize = EEPROM512_SIZE; break;
    }
    if (!saveData_ && expectedSize != 0) {
        saveData_ = std::make_shared<std::vector<uint8_t>>(expectedSize, 0xFF);
        if (saveType_ == SaveType::Eeprom)
            saveData_->reserve(EEPROM_SIZE);
    } else if (saveData_) {
        const bool validEepromSize = saveType_ == SaveType::Eeprom &&
            (saveData_->size() == EEPROM512_SIZE ||
             saveData_->size() == EEPROM_SIZE);
        if (saveType_ == SaveType::None ||
            (saveData_->size() != expectedSize && !validEepromSize))
            throw std::invalid_argument("GBA save data size does not match ROM save hardware");
        if (saveType_ == SaveType::Eeprom)
            saveData_->reserve(EEPROM_SIZE);
    }
    reset();
}

GbaSystem::SaveType GbaSystem::detectSaveType(const std::vector<uint8_t> &rom) {
    const auto hasMarker = [&rom](const char *marker) {
        const size_t length = std::char_traits<char>::length(marker);
        return std::search(rom.begin(), rom.end(), marker,
                           marker + length) != rom.end();
    };
    if (hasMarker("FLASH1M_V"))
        return SaveType::Flash128;
    if (hasMarker("EEPROM_V"))
        return SaveType::Eeprom;
    if (hasMarker("SRAM_V"))
        return SaveType::Sram;
    if (hasMarker("FLASH512_V") || hasMarker("FLASH_V"))
        return SaveType::Flash64;
    return SaveType::None;
}

bool GbaSystem::hasRtcMarker(const std::vector<uint8_t> &rom) {
    constexpr char marker[] = "SIIRTC_V";
    return std::search(rom.begin(), rom.end(), marker,
                       marker + sizeof(marker) - 1) != rom.end();
}

bool GbaSystem::isEepromAddress(uint32_t address) {
    return address >= EEPROM_BASE && address < EEPROM_BASE + EEPROM_WINDOW_SIZE;
}

uint8_t *GbaSystem::getSaveRamData() {
    return saveData_ && !saveData_->empty() ? saveData_->data() : nullptr;
}

size_t GbaSystem::getSaveRamSize() const {
    return saveData_ ? saveData_->size() : 0;
}

std::shared_ptr<std::vector<uint8_t>> GbaSystem::getSaveRamStorage() const {
    return saveData_;
}

uint8_t *GbaSystem::getRtcData() {
    return rtc_.storageData();
}

size_t GbaSystem::getRtcSize() const {
    return rtc_.storageSize();
}

std::shared_ptr<std::vector<uint8_t>> GbaSystem::getRtcStorage() const {
    return rtc_.storage();
}

std::vector<int16_t> GbaSystem::takeAudioSamples() {
    return audio_.takeSamples();
}

void GbaSystem::beginCpuStep() {
    cpuStepActive_ = true;
    cpuStepFirstBusAccess_ = true;
    cpuStepWaitCycles_ = 0;
}

uint32_t GbaSystem::endCpuStep(uint32_t baseCycles) {
    cpuStepActive_ = false;
    const uint32_t cycles = baseCycles + cpuStepWaitCycles_;
    cpuStepWaitCycles_ = 0;
    return cycles;
}

uint32_t GbaSystem::gamePakAccessCycles(unsigned region,
                                       bool sequential) const {
    const uint16_t waitcnt = static_cast<uint16_t>(
        io_[REG_WAITCNT - 0x04000000] |
        (static_cast<uint16_t>(io_[REG_WAITCNT - 0x04000000 + 1] & 0x5F)
         << 8));
    constexpr uint32_t WAIT_CYCLES[] = {4, 3, 2, 8};
    constexpr uint32_t BUS_CYCLE = 1;
    if (region >= 3) {
        return BUS_CYCLE + WAIT_CYCLES[waitcnt & 3];
    }
    if (!sequential) {
        const unsigned shift = 2 + region * 3;
        return BUS_CYCLE + WAIT_CYCLES[(waitcnt >> shift) & 3];
    }

    const unsigned secondAccessBit = 4 + region * 3;
    const uint32_t waitCycles = ((waitcnt >> secondAccessBit) & 1) != 0
        ? 1
        : region == 0 ? 2 : region == 1 ? 4 : 8;
    return BUS_CYCLE + waitCycles;
}

void GbaSystem::recordCpuBusAccess(uint32_t address, unsigned width) const {
    if (!cpuStepActive_)
        return;

    const bool gamePakRom = address >= 0x08000000 && address < 0x0E000000;
    const bool gamePakRam = address >= FLASH_BASE &&
                            address < FLASH_BASE + FLASH_WINDOW_SIZE;
    if (!gamePakRom && !gamePakRam) {
        cpuStepFirstBusAccess_ = false;
        lastGamePakAccessValid_ = false;
        return;
    }

    const auto recordTransaction = [this](uint32_t transactionAddress,
                                          unsigned transactionWidth,
                                          unsigned region) {
        const bool sequential = lastGamePakAccessValid_ &&
            lastGamePakAccessRegion_ == region &&
            lastGamePakAccessAddress_ + lastGamePakAccessWidth_ ==
                transactionAddress;
        const uint32_t accessCycles = gamePakAccessCycles(region, sequential);
        if (cpuStepFirstBusAccess_) {
            cpuStepWaitCycles_ += accessCycles - 1;
            cpuStepFirstBusAccess_ = false;
        } else {
            cpuStepWaitCycles_ += accessCycles;
        }
        lastGamePakAccessValid_ = true;
        lastGamePakAccessAddress_ = transactionAddress;
        lastGamePakAccessWidth_ = transactionWidth;
        lastGamePakAccessRegion_ = region;
    };

    if (gamePakRam) {
        for (unsigned byte = 0; byte < width; ++byte)
            recordTransaction(address + byte, 1, 3);
        return;
    }

    const uint32_t firstHalfword = address & ~uint32_t{1};
    const unsigned halfwords = width == 4 ? 2 : 1;
    for (unsigned halfword = 0; halfword < halfwords; ++halfword) {
        const uint32_t transactionAddress = firstHalfword + halfword * 2;
        const unsigned region = (transactionAddress >> 25) & 3;
        recordTransaction(transactionAddress, 2, region);
    }
}

void GbaSystem::reset() {
    const bool wasMultiplayer = inMultiplayerMode();
    abortMultiplayerTransfer();
    multiplayerError_ = false;
    linkedFrameCarry_ = 0;
    remoteFrameCycles_ = 0;
    emulatedCycles_ = 0;
    remotePeerCycleOffset_ = 0;
    remotePredictionUsed_ = false;
    normalSerialBusy_ = false;
    normalSerialCycles_ = 0;
    externalWorkRam_.fill(0);
    internalWorkRam_.fill(0);
    io_.fill(0);
    if (wasMultiplayer && remoteLinkConnected_ && sendRemoteReady_)
        notifyRemoteReadiness(false);
    remoteLocalReady_ = false;
    remoteLinkHot_ = false;
    remotePredictionUsed_ = false;
    remoteFrameStalled_ = false;
    remotePredictedWords_.clear();
    paletteRam_.fill(0);
    videoRam_.fill(0);
    oam_.fill(0);
    renderedOam_.fill(0);
    renderedOamInitialized_ = false;
    dmaSource_.fill(0);
    dmaDestination_.fill(0);
    dmaInitialized_.fill(false);
    dmaActive_ = false;
    timerReload_.fill(0);
    timerCounter_.fill(0);
    timerPrescalerCycles_.fill(0);
    cpuStepActive_ = false;
    cpuStepFirstBusAccess_ = true;
    cpuStepWaitCycles_ = 0;
    lastGamePakAccessValid_ = false;
    lastGamePakAccessAddress_ = 0;
    lastGamePakAccessWidth_ = 0;
    lastGamePakAccessRegion_ = 0;
    audio_.reset();
    flashCommandState_ = FlashCommandState::Ready;
    flashBank_ = 0;
    flashIdMode_ = false;
    eepromInputBits_.clear();
    eepromAddress_ = 0;
    eepromReadCursor_ = 0;
    eepromReadActive_ = false;
    eepromDmaWriteActive_ = false;
    rtc_.resetPort();
    for (unsigned object = 0; object < 128; ++object)
        oam_[object * 8 + 1] = 0x02; // HLE BIOS leaves regular objects disabled.
    frameBuffer_.fill(0x7FFF);
    pressedButtons_ = 0;
    remoteInputLatch_ = 0;
    remoteFrameStalled_ = false;
    frameCycle_ = 0;
    renderedScanline_ = 0xFFFF;
    renderedPixel_ = 0;
    write16(0x04000020, 0x0100); // HLE BIOS starts affine layers at 1:1.
    write16(0x04000026, 0x0100);
    write16(0x04000030, 0x0100);
    write16(0x04000036, 0x0100);
    if (bios_.empty()) {
        // HLE post-BIOS cartridge handoff: enter in System/ARM state with
        // the BIOS-initialized mode stacks and post-boot flag already set.
        cpu_.reset(0x08000000, 0x03007F00, 0xDF, 0x03007FA0,
                   0x03007FE0);
        io_[REG_POSTFLG - 0x04000000] = 1;
    } else {
        cpu_.reset(0, 0, 0xD3); // ARM reset state: Supervisor with IRQ/FIQ masked.
    }
    updateDisplayStatus();
}

void GbaSystem::setPressedButtons(uint16_t activeHighMask) {
    const uint16_t buttons = activeHighMask & 0x03FF;
    if (remoteLinkEnabled_ || remoteFrameStalled_)
        remoteInputLatch_ |= buttons;
    pressedButtons_ = buttons;
}

uint32_t GbaSystem::videoRamOffset(uint32_t address) {
    uint32_t offset = address & 0x1FFFF;
    if (offset >= 0x18000)
        offset = 0x10000 + (offset - 0x18000);
    return offset;
}

uint16_t GbaSystem::readOamForRendering(uint32_t address) const {
    const size_t offset = (address - OAM_BASE) & 0x3FE;
    return static_cast<uint16_t>(renderedOam_[offset] |
        (static_cast<uint16_t>(renderedOam_[offset + 1]) << 8));
}

uint8_t GbaSystem::readFlash(uint32_t address) const {
    if (!saveData_)
        return 0xFF;

    const uint32_t offset = address - FLASH_BASE;
    if (flashIdMode_) {
        if (offset == 0)
            return saveType_ == SaveType::Flash64 ? 0x32 : 0xC2;
        if (offset == 1)
            return saveType_ == SaveType::Flash64 ? 0x1B : 0x09;
        return 0xFF;
    }

    return (*saveData_)[static_cast<size_t>(flashBank_) * FLASH_BANK_SIZE +
                        (offset & (FLASH_WINDOW_SIZE - 1))];
}

void GbaSystem::writeFlash(uint32_t address, uint8_t value) {
    if (!saveData_)
        return;

    const uint32_t offset = address - FLASH_BASE;
    if (value == 0xF0 && flashCommandState_ != FlashCommandState::ProgramData &&
        flashCommandState_ != FlashCommandState::BankData) {
        flashCommandState_ = FlashCommandState::Ready;
        flashIdMode_ = false;
        return;
    }

    switch (flashCommandState_) {
    case FlashCommandState::Ready:
        if (offset == 0x5555 && value == 0xAA)
            flashCommandState_ = FlashCommandState::UnlockSecond;
        break;
    case FlashCommandState::UnlockSecond:
        flashCommandState_ = offset == 0x2AAA && value == 0x55
            ? FlashCommandState::Command
            : FlashCommandState::Ready;
        break;
    case FlashCommandState::Command:
        if (offset != 0x5555) {
            flashCommandState_ = FlashCommandState::Ready;
            break;
        }
        switch (value) {
        case 0x90:
            flashIdMode_ = true;
            flashCommandState_ = FlashCommandState::Ready;
            break;
        case 0xA0:
            flashCommandState_ = FlashCommandState::ProgramData;
            break;
        case 0x80:
            flashCommandState_ = FlashCommandState::EraseUnlockFirst;
            break;
        case 0xB0:
            flashCommandState_ = FlashCommandState::BankData;
            break;
        default:
            flashCommandState_ = FlashCommandState::Ready;
            break;
        }
        break;
    case FlashCommandState::EraseUnlockFirst:
        flashCommandState_ = offset == 0x5555 && value == 0xAA
            ? FlashCommandState::EraseUnlockSecond
            : FlashCommandState::Ready;
        break;
    case FlashCommandState::EraseUnlockSecond:
        flashCommandState_ = offset == 0x2AAA && value == 0x55
            ? FlashCommandState::EraseCommand
            : FlashCommandState::Ready;
        break;
    case FlashCommandState::EraseCommand:
        if (value == 0x10 && offset == 0x5555) {
            std::fill(saveData_->begin(), saveData_->end(), 0xFF);
        } else if (value == 0x30) {
            const uint32_t sectorOffset = offset & ~(FLASH_SECTOR_SIZE - 1);
            const size_t start = saveType_ == SaveType::Flash128
                ? static_cast<size_t>(flashBank_) * FLASH_BANK_SIZE + sectorOffset
                : sectorOffset;
            std::fill(saveData_->begin() + static_cast<std::ptrdiff_t>(start),
                      saveData_->begin() + static_cast<std::ptrdiff_t>(
                          start + FLASH_SECTOR_SIZE), 0xFF);
        }
        flashCommandState_ = FlashCommandState::Ready;
        flashIdMode_ = false;
        break;
    case FlashCommandState::ProgramData: {
        const size_t index = (saveType_ == SaveType::Flash128
            ? static_cast<size_t>(flashBank_) * FLASH_BANK_SIZE : 0) + offset;
        (*saveData_)[index] &= value;
        flashCommandState_ = FlashCommandState::Ready;
        flashIdMode_ = false;
        break;
    }
    case FlashCommandState::BankData:
        if (offset == 0)
            flashBank_ = saveType_ == SaveType::Flash128 ? value & 1 : 0;
        flashCommandState_ = FlashCommandState::Ready;
        flashIdMode_ = false;
        break;
    }
}

uint16_t GbaSystem::readEeprom() const {
    if (!eepromReadActive_)
        return 1;
    if (eepromReadCursor_ >= 68)
        return 1;
    if (eepromReadCursor_ < 4) {
        ++eepromReadCursor_;
        return 0;
    }

    const unsigned dataBit = eepromReadCursor_ - 4;
    ++eepromReadCursor_;
    if (eepromReadCursor_ == 68)
        eepromReadActive_ = false;
    const size_t byteIndex = static_cast<size_t>(eepromAddress_) * 8 +
                             dataBit / 8;
    if (!saveData_ || byteIndex >= saveData_->size())
        return 1;
    const uint8_t value = (*saveData_)[byteIndex];
    return static_cast<uint16_t>((value >> (7 - (dataBit & 7))) & 1);
}

void GbaSystem::writeEeprom(uint16_t value) {
    if (eepromDmaWriteActive_)
        eepromInputBits_.push_back(static_cast<uint8_t>(value & 1));
}

void GbaSystem::finishEepromDma(uint32_t transferCount) {
    eepromDmaWriteActive_ = false;
    if (eepromInputBits_.size() != transferCount || eepromInputBits_.size() < 9 ||
        eepromInputBits_[0] != 1)
        return;

    const unsigned command =
        (static_cast<unsigned>(eepromInputBits_[1]) << 1) | eepromInputBits_[2];
    const bool writeCommand = command == 2;
    const bool readCommand = command == 3;
    if (!writeCommand && !readCommand)
        return;

    const bool largeAddress = transferCount == 17 || transferCount == 81;
    const unsigned addressBits = largeAddress ? 14 : 6;
    if ((writeCommand && transferCount != 73 && transferCount != 81) ||
        (readCommand && transferCount != 9 && transferCount != 17))
        return;
    if (saveType_ == SaveType::Eeprom && largeAddress &&
        saveData_->size() == EEPROM512_SIZE) {
        saveData_->resize(EEPROM_SIZE, 0xFF);
    }

    size_t cursor = 3;
    uint16_t blockAddress = 0;
    for (unsigned bit = 0; bit < addressBits; ++bit)
        blockAddress = static_cast<uint16_t>((blockAddress << 1) |
                                              eepromInputBits_[cursor++]);
    eepromAddress_ = blockAddress;

    if (writeCommand) {
        const size_t firstByte = static_cast<size_t>(blockAddress) * 8;
        if (!saveData_ || firstByte + 8 > saveData_->size())
            return;
        for (size_t byte = 0; byte < 8; ++byte) {
            uint8_t value = 0;
            for (unsigned bit = 0; bit < 8; ++bit)
                value = static_cast<uint8_t>((value << 1) |
                    eepromInputBits_[cursor++]);
            (*saveData_)[firstByte + byte] = value;
        }
        eepromReadCursor_ = 0;
        eepromReadActive_ = false;
    } else {
        eepromReadCursor_ = 0;
        eepromReadActive_ = true;
    }
}

uint8_t GbaSystem::read8(uint32_t address) const {
    recordCpuBusAccess(address, 1);
    return read8Impl(address);
}

uint8_t GbaSystem::read8Impl(uint32_t address) const {
    if (address < bios_.size())
        return bios_[address];
    if (address >= 0x02000000 && address < 0x03000000)
        return externalWorkRam_[(address - 0x02000000) & 0x3FFFF];
    if (address >= 0x03000000 && address < 0x04000000)
        return internalWorkRam_[(address - 0x03000000) & 0x7FFF];
    if (address >= 0x04000000 && address < 0x04000400) {
        if ((address == REG_SIOCNT || address == REG_RCNT) && inMultiplayerMode()) {
            if (address == REG_SIOCNT) {
                const auto control = io_[address - 0x04000000] & 3;
                const auto status = (multiplayerChild() ? 0x14 : 0) |
                    (multiplayerReady() ? 0x08 : 0) |
                    (multiplayerError_ ? 0x40 : 0) |
                    (multiplayerBusy_ ? SIO_START : 0);
                return static_cast<uint8_t>(control | status);
            }
            if (address == REG_RCNT) {
                const auto reserved = io_[address - 0x04000000] & 0xF0;
                const auto lines = (multiplayerBusy_ ? 0 : 0x09) |
                    (multiplayerReady() ? 0x02 : 0) |
                    (multiplayerChild() ? 0x04 : 0);
                return static_cast<uint8_t>(reserved | lines);
            }
        }
        if (audio_.handlesRegister(address))
            return audio_.readRegister(address);
        if (address == REG_WAITCNT + 1)
            return static_cast<uint8_t>(
                io_[address - 0x04000000] & 0x5F);
        if (address == REG_KEYINPUT || address == REG_KEYINPUT + 1) {
            const uint16_t value = static_cast<uint16_t>(
                ~(pressedButtons_ | remoteInputLatch_)) & 0x03FF;
            return static_cast<uint8_t>(value >> ((address - REG_KEYINPUT) * 8));
        }
        if (address == REG_IE + 1 || address == REG_IF + 1)
            return static_cast<uint8_t>(io_[address - 0x04000000] & 0x3F);
        if (address == REG_IME)
            return static_cast<uint8_t>(io_[address - 0x04000000] & 1);
        if (address == REG_IME + 1)
            return 0;
        if (address >= TIMER_BASE && address < TIMER_BASE + 16) {
            const uint32_t timerOffset = address - TIMER_BASE;
            if ((timerOffset & 3) < 2) {
                const uint16_t counter = timerCounter_[timerOffset / 4];
                return static_cast<uint8_t>(counter >> ((timerOffset & 1) * 8));
            }
        }
        return io_[address - 0x04000000];
    }
    if (address >= 0x05000000 && address < 0x06000000)
        return paletteRam_[(address - 0x05000000) & 0x3FF];
    if (address >= 0x06000000 && address < 0x07000000)
        return videoRam_[videoRamOffset(address)];
    if (address >= 0x07000000 && address < 0x08000000)
        return oam_[(address - 0x07000000) & 0x3FF];
    if (address >= FLASH_BASE && address < FLASH_BASE + FLASH_WINDOW_SIZE) {
        if (saveType_ == SaveType::Sram)
            return (*saveData_)[(address - FLASH_BASE) & (SRAM_SIZE - 1)];
        if (saveType_ == SaveType::Flash64 || saveType_ == SaveType::Flash128)
            return readFlash(address);
        return 0xFF;
    }
    if (address >= 0x08000000 && address < 0x0E000000 && !rom_.empty()) {
        const size_t offset = (address & 0x01FFFFFF) % rom_.size();
        return rtc_.readByte(address, rom_[offset]);
    }
    return 0;
}

uint16_t GbaSystem::read16(uint32_t address) const {
    address &= ~uint32_t{1};
    recordCpuBusAccess(address, 2);
    return read16Impl(address);
}

uint16_t GbaSystem::read16Impl(uint32_t address) const {
    if (saveType_ == SaveType::Eeprom && isEepromAddress(address))
        return readEeprom();
    return static_cast<uint16_t>(read8Impl(address) |
        (static_cast<uint16_t>(read8Impl(address + 1)) << 8));
}

uint32_t GbaSystem::read32(uint32_t address) const {
    address &= ~uint32_t{3};
    recordCpuBusAccess(address, 4);
    return static_cast<uint32_t>(read16Impl(address)) |
           (static_cast<uint32_t>(read16Impl(address + 2)) << 16);
}

void GbaSystem::write8(uint32_t address, uint8_t value) {
    recordCpuBusAccess(address, 1);
    write8Impl(address, value);
}

void GbaSystem::write8Impl(uint32_t address, uint8_t value) {
    if (address >= 0x02000000 && address < 0x03000000) {
        externalWorkRam_[(address - 0x02000000) & 0x3FFFF] = value;
        return;
    }
    if (address >= 0x03000000 && address < 0x04000000) {
        internalWorkRam_[(address - 0x03000000) & 0x7FFF] = value;
        return;
    }
    if (audio_.writeFifo(address, value))
        return;
    if (audio_.writeRegister(address, value)) {
        io_[address - 0x04000000] = value;
        return;
    }
    if (address >= 0x04000000 && address < 0x04000400) {
        if ((address & ~1u) == REG_SIOCNT || (address & ~1u) == REG_RCNT) {
            const uint32_t aligned = address & ~1u;
            const unsigned shift = (address & 1u) * 8;
            const auto offset = aligned - 0x04000000;
            const unsigned stored = io_[offset] |
                (static_cast<unsigned>(io_[offset + 1]) << 8);
            const auto merged = static_cast<uint16_t>(
                (stored & ~(0xFFu << shift)) | (static_cast<unsigned>(value) << shift));
            writeSerialControl(aligned, merged);
            return;
        }
        if (address <= 0x04000056)
            flushCurrentScanline();
        if (address == REG_KEYINPUT || address == REG_KEYINPUT + 1)
            return;
        if (address == REG_VCOUNT || address == REG_VCOUNT + 1)
            return;
        if (address == REG_DISPSTAT) {
            auto &stored = io_[address - 0x04000000];
            stored = static_cast<uint8_t>((stored & ~0x38u) | (value & 0x38u));
            return;
        }
        if (address == REG_DISPSTAT + 1) {
            io_[address - 0x04000000] = value;
            return;
        }
        if (address == REG_IE || address == REG_IE + 1) {
            io_[address - 0x04000000] = address == REG_IE
                ? value
                : static_cast<uint8_t>(value & 0x3F);
            return;
        }
        if (address == REG_IF || address == REG_IF + 1) {
            io_[address - 0x04000000] = static_cast<uint8_t>(
                io_[address - 0x04000000] & ~value);
            return;
        }
        if (address == REG_IME) {
            io_[address - 0x04000000] = value & 1;
            return;
        }
        if (address == REG_IME + 1)
            return;
        if (address == REG_WAITCNT) {
            io_[address - 0x04000000] = value;
            return;
        }
        if (address == REG_WAITCNT + 1) {
            io_[address - 0x04000000] = value & 0x5F;
            return;
        }
        if (address >= TIMER_BASE && address < TIMER_BASE + 16) {
            const uint32_t timerOffset = address - TIMER_BASE;
            const unsigned timer = timerOffset / 4;
            const unsigned registerByte = timerOffset & 3;
            if (registerByte < 2) {
                const unsigned shift = registerByte * 8;
                timerReload_[timer] = static_cast<uint16_t>(
                    (timerReload_[timer] & ~(0xFFu << shift)) |
                    (static_cast<uint32_t>(value) << shift));
                return;
            }
            if (registerByte == 2) {
                const uint16_t oldControl = read16(TIMER_BASE + timer * 4 + 2);
                uint8_t control = value & 0xC7;
                if (timer == 0)
                    control &= static_cast<uint8_t>(~0x04u);
                io_[address - 0x04000000] = control;
                const bool wasEnabled = (oldControl & 0x0080) != 0;
                const bool enabled = (control & 0x80) != 0;
                if (!wasEnabled && enabled) {
                    timerCounter_[timer] = timerReload_[timer];
                    timerPrescalerCycles_[timer] = 0;
                } else if ((oldControl & 3) != (control & 3)) {
                    timerPrescalerCycles_[timer] = 0;
                }
                return;
            }
            io_[address - 0x04000000] = 0;
            return;
        }
        io_[address - 0x04000000] = value;
        if (!dmaActive_) {
            for (unsigned channel = 0; channel < 4; ++channel) {
                const uint32_t controlHighByte =
                    0x040000BB + channel * 12;
                if (address == controlHighByte) {
                    dmaInitialized_[channel] = false;
                    triggerDma(channel, 0);
                    break;
                }
            }
        }
        return;
    }
    if (address >= PALETTE_BASE && address < 0x06000000) {
        flushCurrentScanline();
        const uint32_t offset = (address - PALETTE_BASE) & 0x3FE;
        paletteRam_[offset] = value;
        paletteRam_[offset + 1] = value;
        return;
    }
    if (address >= 0x06000000 && address < 0x07000000) {
        flushCurrentScanline();
        const uint32_t offset = videoRamOffset(address);
        const unsigned mode = read16(REG_DISPCNT) & 7;
        const uint32_t objectBase = mode >= 3 && mode <= 5 ? 0x14000 : 0x10000;
        if (offset >= objectBase)
            return;
        const uint32_t alignedAddress = address & ~uint32_t{1};
        videoRam_[videoRamOffset(alignedAddress)] = value;
        videoRam_[videoRamOffset(alignedAddress + 1)] = value;
        return;
    }
    if (address >= OAM_BASE && address < 0x08000000)
        return;
    if (address >= FLASH_BASE && address < FLASH_BASE + FLASH_WINDOW_SIZE) {
        if (saveType_ == SaveType::Sram) {
            (*saveData_)[(address - FLASH_BASE) & (SRAM_SIZE - 1)] = value;
            return;
        }
        if (saveType_ == SaveType::Flash64 || saveType_ == SaveType::Flash128)
            writeFlash(address, value);
        return;
    }
}

void GbaSystem::write16(uint32_t address, uint16_t value) {
    address &= ~uint32_t{1};
    recordCpuBusAccess(address, 2);
    write16Impl(address, value);
}

void GbaSystem::write16Impl(uint32_t address, uint16_t value) {
    if (address == REG_SIOCNT || address == REG_RCNT) {
        writeSerialControl(address, value);
        return;
    }
    if (rtc_.handlesAddress(address)) {
        rtc_.writeRegister(address, value);
        return;
    }
    if (saveType_ == SaveType::Eeprom && isEepromAddress(address)) {
        writeEeprom(value);
        return;
    }
    if (address >= PALETTE_BASE && address < 0x06000000) {
        flushCurrentScanline();
        const uint32_t offset = (address - PALETTE_BASE) & 0x3FE;
        paletteRam_[offset] = static_cast<uint8_t>(value);
        paletteRam_[offset + 1] = static_cast<uint8_t>(value >> 8);
        return;
    }
    if (address >= VRAM_BASE && address < 0x07000000) {
        flushCurrentScanline();
        videoRam_[videoRamOffset(address)] = static_cast<uint8_t>(value);
        videoRam_[videoRamOffset(address + 1)] = static_cast<uint8_t>(value >> 8);
        return;
    }
    if (address >= OAM_BASE && address < 0x08000000) {
        flushCurrentScanline();
        const uint32_t offset = (address - OAM_BASE) & 0x3FE;
        oam_[offset] = static_cast<uint8_t>(value);
        oam_[offset + 1] = static_cast<uint8_t>(value >> 8);
        return;
    }
    write8Impl(address, static_cast<uint8_t>(value));
    write8Impl(address + 1, static_cast<uint8_t>(value >> 8));
}

void GbaSystem::write32(uint32_t address, uint32_t value) {
    address &= ~uint32_t{3};
    recordCpuBusAccess(address, 4);
    write16Impl(address, static_cast<uint16_t>(value));
    write16Impl(address + 2, static_cast<uint16_t>(value >> 16));
}

void GbaSystem::registerRamReset(uint8_t flags) {
    // RegisterRamReset always enters forced blank, even when no reset region
    // was selected.
    write16(REG_DISPCNT, 0x0080);

    if ((flags & 0x01) != 0)
        externalWorkRam_.fill(0);
    if ((flags & 0x02) != 0)
        std::fill(internalWorkRam_.begin(), internalWorkRam_.end() - 0x200, 0);
    if ((flags & 0x04) != 0)
        paletteRam_.fill(0);
    if ((flags & 0x08) != 0)
        videoRam_.fill(0);
    if ((flags & 0x10) != 0)
        oam_.fill(0);

    if ((flags & 0x20) != 0) {
        write16(REG_SIOCNT, 0);
        write16(REG_RCNT, 0x8000); // Initial GPIO state.
        write16(0x04000120, 0); // SIOMLT_SEND
        write16(0x04000140, 0); // JOYCNT
        write32(0x04000150, 0); // JOY_RECV_L/H
        write32(0x04000158, 0); // JOY_TRANS_L/H
    }

    if ((flags & 0x40) != 0) {
        std::fill(io_.begin() + 0x060, io_.begin() + 0x0A8, 0);
        write16(0x04000088, 0x0200); // SOUNDBIAS reset value
    }

    if ((flags & 0x80) != 0) {
        // Reset display control and the video, DMA, timer, and interrupt
        // registers that the BIOS reset routine owns.
        std::fill(io_.begin() + 0x008, io_.begin() + 0x056, 0);
        write16(0x04000020, 0x0100); // BG2PA
        write16(0x04000026, 0x0100); // BG2PD
        write16(0x04000030, 0x0100); // BG3PA
        write16(0x04000036, 0x0100); // BG3PD
        std::fill(io_.begin() + 0x0B0, io_.begin() + 0x0E0, 0);
        dmaInitialized_.fill(false);
        std::fill(io_.begin() + 0x100, io_.begin() + 0x110, 0);
        timerReload_.fill(0);
        timerCounter_.fill(0);
        timerPrescalerCycles_.fill(0);
        write16(REG_IE, 0);
        io_[REG_IF - 0x04000000] = 0;
        io_[REG_IF - 0x04000000 + 1] = 0;
        write16(REG_IME, 0);
        write16(REG_DISPCNT, 0x0080);
        updateDisplayStatus();
    }
}

GbaSystem::~GbaSystem() {
    disconnectLink();
}

void GbaSystem::connectLinkChild(GbaSystem &child) {
    if (&child == this || linkPeer_ || child.linkPeer_)
        throw std::invalid_argument("GBA link requires two unconnected systems");
    linkPeer_ = &child;
    child.linkPeer_ = this;
    child.linkChild_ = true;
    multiplayerError_ = child.multiplayerError_ = false;
}

void GbaSystem::disconnectLink() {
    if (!linkPeer_)
        return;
    abortMultiplayerTransfer();
    linkPeer_->linkPeer_ = nullptr;
    linkPeer_->linkChild_ = false;
    linkPeer_->linkedFrameCarry_ = 0;
    linkPeer_ = nullptr;
    linkChild_ = false;
    linkedFrameCarry_ = 0;
}

void GbaSystem::configureRemoteMultiplayerLink(
    bool child, bool connected, MultiplayerStartSender sendStart,
    MultiplayerWordSender sendWord, MultiplayerReadinessSender sendReady,
    MultiplayerFrameSender sendFrame,
    MultiplayerWordPredictor predictWord) {
    if (linkPeer_ || remoteLinkEnabled_)
        throw std::logic_error("GBA already has a multiplayer link");
    remoteLinkEnabled_ = true;
    remoteLinkChild_ = child;
    remoteLinkConnected_ = connected;
    remotePeerReady_ = false;
    sendRemoteStart_ = std::move(sendStart);
    sendRemoteWord_ = std::move(sendWord);
    sendRemoteReady_ = std::move(sendReady);
    sendRemoteFrame_ = std::move(sendFrame);
    predictRemoteWord_ = std::move(predictWord);
    remotePredictedWords_.clear();
    remotePendingWords_.clear();
    remotePeerWordReady_ = false;
    remoteReplySent_ = false;
    remotePendingStarts_.clear();
    remoteTransferSequence_ = 0;
    remoteActiveSequence_ = 0;
    remoteFrameCount_ = 0;
    remotePeerFrameCount_ = 0;
    remoteLocalReadyFrame_ = 0;
    remotePeerReadyFrame_ = 0;
    remotePeerFrameKnown_ = false;
    remoteLocalReady_ = false;
    remoteLinkHot_ = false;
    remotePredictionUsed_ = false;
    multiplayerError_ = false;
    if (remoteLinkConnected_ && inMultiplayerMode())
        notifyRemoteReadiness(true);
}

void GbaSystem::setRemoteMultiplayerEventPump(
    RemoteMultiplayerEventPump pump) {
    if (!remoteLinkEnabled_)
        throw std::logic_error("GBA has no remote multiplayer link");
    remoteEventPump_ = std::move(pump);
}

void GbaSystem::setRemoteMultiplayerConnected(bool connected) {
    if (!remoteLinkEnabled_)
        return;
    remoteLinkConnected_ = connected;
    if (!connected) {
        remotePeerReady_ = false;
        remotePeerFrameKnown_ = false;
        remoteLinkHot_ = false;
        remotePeerCycleOffset_ = 0;
        remotePredictionUsed_ = false;
    } else if (inMultiplayerMode()) {
        notifyRemoteReadiness(true);
    }
    if (!connected)
        multiplayerError_ = true;
    if (!connected)
        abortMultiplayerTransfer();
}

void GbaSystem::notifyRemoteReadiness(bool ready) {
    remoteLocalReady_ = ready;
    if (ready) {
        remoteLocalReadyFrame_ = remoteFrameCount_;
        remoteLocalReadyCycle_ = emulatedCycles_;
        remotePeerCycleOffset_ = 0;
    } else {
        remoteLinkHot_ = false;
    }
    if (sendRemoteReady_)
        sendRemoteReady_(ready);
}

void GbaSystem::setRemoteMultiplayerReady(bool ready, uint64_t peerFrame) {
    if (!remoteLinkEnabled_ || !remoteLinkConnected_)
        return;
    remotePeerReady_ = ready;
    if (ready) {
        remotePeerReadyFrame_ = std::max(peerFrame, remotePeerFrameCount_);
        if (!remotePeerFrameKnown_ || peerFrame > remotePeerFrameCount_)
            remotePeerFrameCount_ = peerFrame;
        remotePeerFrameKnown_ = true;
    } else {
        remoteLinkHot_ = false;
    }
}

bool GbaSystem::receiveRemoteMultiplayerFrame(uint64_t peerFrame) {
    if (!remoteLinkEnabled_ || !remoteLinkConnected_ ||
        (remotePeerFrameKnown_ && peerFrame < remotePeerFrameCount_))
        return false;
    remotePeerFrameCount_ = peerFrame;
    remotePeerFrameKnown_ = true;
    return true;
}

void GbaSystem::markRemoteLinkActivity() {
    if (!remoteLinkEnabled_ || !remoteLocalReady_ || !remotePeerReady_)
        return;
    if (!remoteLinkHot_) {
        remoteHotLocalBase_ = remoteFrameCount_ - remoteLocalReadyFrame_;
        remoteHotPeerBase_ = remotePeerFrameCount_ - remotePeerReadyFrame_;
        remoteLinkHot_ = true;
    }
    remoteLastActivityFrame_ = remoteFrameCount_;
}

bool GbaSystem::shouldPaceRemoteFrame() {
    // A pending or active transfer must be allowed to advance to its local
    // SIO completion point. Once the local wire time has elapsed, the CPU may
    // wait on the busy bit for a bounded number of VBlanks while the peer reply
    // is in transit.
    if (!remotePendingStarts_.empty() ||
        (multiplayerBusy_ && multiplayerCycles_ != 0))
        return false;
    // Once a compatibility prediction has let the local guest consume a
    // transfer before its peer packet arrives, do not then stop that guest
    // clock on the frontend-frame watermark. The two endpoints are reconciled
    // by the ordered SIO stream and late-word verification instead.
    if (remotePredictionUsed_)
        return false;
    if (!remoteLinkEnabled_ || !remoteLinkConnected_ || !remoteLocalReady_ ||
        !remotePeerReady_ || !remotePeerFrameKnown_ || !remoteLinkHot_)
        return false;
    if (remoteFrameCount_ - remoteLastActivityFrame_ >=
        REMOTE_LINK_HOT_WINDOW_FRAMES) {
        remoteLinkHot_ = false;
        return false;
    }
    const uint64_t localProgress =
        (remoteFrameCount_ - remoteLocalReadyFrame_) - remoteHotLocalBase_;
    const uint64_t peerProgress =
        (remotePeerFrameCount_ - remotePeerReadyFrame_) - remoteHotPeerBase_;
    return localProgress > peerProgress + REMOTE_LINK_MAX_FRAME_LEAD;
}

bool GbaSystem::receiveRemoteMultiplayerStart(uint32_t sequence,
                                              uint16_t parentWord,
                                              uint8_t baudIndex,
                                              uint64_t elapsedCycles) {
    if (!remoteLinkEnabled_ || !remoteLinkChild_ || !remoteLinkConnected_ ||
        !inMultiplayerMode() ||
        sequence != remoteTransferSequence_ + 1 || !sendRemoteWord_ ||
        baudIndex >= 4 ||
        remotePendingStarts_.size() >= REMOTE_LINK_MAX_QUEUED_STARTS) {
        multiplayerError_ = true;
        return false;
    }
    remoteTransferSequence_ = sequence;
    markRemoteLinkActivity();
    // Transfer starts are scheduled on the child's guest clock relative to
    // each game's entry into multiplayer mode. This retains sub-frame SIO
    // ordering even though the packet itself is delivered between frontend
    // frames.
    const uint64_t maxCycle = std::numeric_limits<uint64_t>::max();
    if (elapsedCycles > maxCycle - remoteLocalReadyCycle_ ||
        remotePeerCycleOffset_ >
            maxCycle - remoteLocalReadyCycle_ - elapsedCycles) {
        multiplayerError_ = true;
        return false;
    }
    uint64_t startCycle = remoteLocalReadyCycle_ + elapsedCycles +
                          remotePeerCycleOffset_;
    if (startCycle < emulatedCycles_) {
        const uint64_t lateBy = emulatedCycles_ - startCycle;
        if (lateBy > maxCycle - remotePeerCycleOffset_) {
            multiplayerError_ = true;
            return false;
        }
        // Preserve the sender's inter-transfer cycle spacing when reliable
        // packets arrive after the client has already advanced past them.
        // Shift this and subsequent peer events by a shared late-arrival bias
        // instead of collapsing old transactions into zero-cycle transfers.
        remotePeerCycleOffset_ += lateBy;
        startCycle = emulatedCycles_;
    }
    remotePendingStarts_.push_back({sequence, parentWord, baudIndex, startCycle});
    return true;
}

void GbaSystem::beginRemoteMultiplayerTransfer() {
    if (multiplayerBusy_ || remotePendingStarts_.empty() ||
        emulatedCycles_ < remotePendingStarts_.front().startCycle)
        return;
    const RemoteMultiplayerStart start = remotePendingStarts_.front();
    remotePendingStarts_.pop_front();
    remoteActiveSequence_ = start.sequence;
    multiplayerWords_ = {start.parentWord,
                         read16Impl(REG_SIOMLT_SEND)};
    const uint32_t baud = MULTIPLAYER_BAUD[start.baudIndex];
    const uint32_t transferCycles = (36 * CPU_CLOCK + baud - 1) / baud;
    const uint64_t transferEndCycle =
        start.startCycle + transferCycles;
    multiplayerCycles_ = transferEndCycle > emulatedCycles_
        ? static_cast<uint32_t>(transferEndCycle - emulatedCycles_) : 0;
    multiplayerBusy_ = true;
    multiplayerError_ = false;
    remotePeerWordReady_ = true;
    remoteReplySent_ = false;
    for (uint32_t slot = 0; slot < 4; ++slot)
        storeIo16(REG_SIOMULTI0 + 2 * slot, 0xFFFF);
    if (remoteLinkChild_ && multiplayerCycles_ == 0) {
        sendRemoteWord_(remoteActiveSequence_, multiplayerWords_[1]);
        remoteReplySent_ = true;
        completeMultiplayerTransfer();
    }
}

bool GbaSystem::receiveRemoteMultiplayerWord(uint32_t sequence,
                                             uint16_t childWord) {
    const auto predicted = remotePredictedWords_.find(sequence);
    if (predicted != remotePredictedWords_.end()) {
        const bool matched = predicted->second == childWord;
        remotePredictedWords_.erase(predicted);
        if (!matched) {
            multiplayerError_ = true;
            return false;
        }
        markRemoteLinkActivity();
        return true;
    }
    if (!remoteLinkEnabled_ || remoteLinkChild_ || !remoteLinkConnected_) {
        multiplayerError_ = true;
        return false;
    }
    if (sequence > remoteActiveSequence_ ||
        (sequence == remoteActiveSequence_ && !multiplayerBusy_)) {
        if (sequence <= remoteActiveSequence_ ||
            sequence - remoteActiveSequence_ > REMOTE_LINK_MAX_QUEUED_WORDS) {
            multiplayerError_ = true;
            return false;
        }
        const auto pending = std::find_if(
            remotePendingWords_.begin(), remotePendingWords_.end(),
            [sequence](const RemoteMultiplayerWord &word) {
                return word.sequence == sequence;
            });
        if (pending != remotePendingWords_.end()) {
            if (pending->word != childWord) {
                multiplayerError_ = true;
                return false;
            }
            markRemoteLinkActivity();
            return true;
        }
        if (remotePendingWords_.size() >= REMOTE_LINK_MAX_QUEUED_WORDS) {
            multiplayerError_ = true;
            return false;
        }
        remotePendingWords_.push_back({sequence, childWord});
        markRemoteLinkActivity();
        return true;
    }
    if (!multiplayerBusy_ || sequence != remoteActiveSequence_ ||
        remotePeerWordReady_) {
        multiplayerError_ = true;
        return false;
    }
    markRemoteLinkActivity();
    multiplayerWords_[1] = childWord;
    remotePeerWordReady_ = true;
    if (multiplayerCycles_ == 0)
        completeMultiplayerTransfer();
    return true;
}

void GbaSystem::applyPendingRemoteWord() {
    if (!remoteLinkEnabled_ || remoteLinkChild_ || !multiplayerBusy_ ||
        remotePeerWordReady_)
        return;
    const auto pending = std::find_if(
        remotePendingWords_.begin(), remotePendingWords_.end(),
        [this](const RemoteMultiplayerWord &word) {
            return word.sequence == remoteActiveSequence_;
        });
    if (pending == remotePendingWords_.end())
        return;
    multiplayerWords_[1] = pending->word;
    remotePendingWords_.erase(pending);
    remotePeerWordReady_ = true;
    markRemoteLinkActivity();
    if (multiplayerCycles_ == 0)
        completeMultiplayerTransfer();
}

void GbaSystem::disconnectRemoteMultiplayerLink() {
    if (!remoteLinkEnabled_)
        return;
    abortMultiplayerTransfer();
    remoteLinkEnabled_ = false;
    remoteLinkChild_ = false;
    remoteLinkConnected_ = false;
    remotePeerReady_ = false;
    remotePeerWordReady_ = false;
    sendRemoteStart_ = {};
    sendRemoteWord_ = {};
    sendRemoteReady_ = {};
    sendRemoteFrame_ = {};
    remoteEventPump_ = {};
    predictRemoteWord_ = {};
    remotePredictedWords_.clear();
    remoteLinkHot_ = false;
    remotePeerCycleOffset_ = 0;
    remotePredictionUsed_ = false;
}

uint16_t GbaSystem::loadIo16(uint32_t address) const {
    const auto offset = address - 0x04000000;
    return static_cast<uint16_t>(io_[offset] |
        (static_cast<uint16_t>(io_[offset + 1]) << 8));
}

void GbaSystem::storeIo16(uint32_t address, uint16_t value) {
    const auto offset = address - 0x04000000;
    io_[offset] = static_cast<uint8_t>(value);
    io_[offset + 1] = static_cast<uint8_t>(value >> 8);
}

bool GbaSystem::inMultiplayerMode() const {
    return !(io_[REG_RCNT - 0x04000000 + 1] & 0x80) &&
        (io_[REG_SIOCNT - 0x04000000 + 1] & 0x30) == 0x20;
}

bool GbaSystem::multiplayerReady() const {
    return (linkPeer_ && linkPeer_->inMultiplayerMode()) ||
        (remoteLinkEnabled_ && remoteLinkConnected_ && remotePeerReady_);
}

bool GbaSystem::multiplayerChild() const {
    return linkChild_ || (remoteLinkEnabled_ && remoteLinkChild_);
}

void GbaSystem::writeSerialControl(uint32_t address, uint16_t value) {
    const bool wasMultiplayer = inMultiplayerMode();
    storeIo16(address, value);
    const bool isMultiplayer = inMultiplayerMode();
    if (remoteLinkEnabled_ && remoteLinkConnected_ &&
        wasMultiplayer != isMultiplayer)
        notifyRemoteReadiness(isMultiplayer);
    if (!isMultiplayer) {
        if (wasMultiplayer)
            abortMultiplayerTransfer();
        const uint16_t control = read16Impl(REG_SIOCNT);
        const bool normalMode = !(read16Impl(REG_RCNT) & 0x8000) &&
            ((control >> 12) & 3) <= 1;
        if (!normalMode) {
            if (normalSerialBusy_)
                storeIo16(REG_SIOCNT, control & ~SIO_START);
            normalSerialBusy_ = false;
            normalSerialCycles_ = 0;
        } else if (address == REG_SIOCNT) {
            if (!(value & SIO_START)) {
                normalSerialBusy_ = false;
                normalSerialCycles_ = 0;
            } else if (!normalSerialBusy_) {
                normalSerialBusy_ = true;
                const uint32_t bits = (control & 0x1000) ? 32 : 8;
                normalSerialCycles_ = (bits * CPU_CLOCK +
                    NORMAL_BAUD[(control >> 1) & 1] - 1) /
                    NORMAL_BAUD[(control >> 1) & 1];
                // With no emulated Normal-mode peripheral, the SI line is
                // pulled high and the receiver shifts in one bits.
                if (!(control & 1))
                    normalSerialCycles_ = 0; // External clock has not arrived.
            }
        }
        return;
    }
    normalSerialBusy_ = false;
    normalSerialCycles_ = 0;
    if (!wasMultiplayer)
        multiplayerError_ = false;
    storeIo16(REG_SIOCNT, read16Impl(REG_SIOCNT) & SIO_MULTI_WRITABLE);
    if (address != REG_SIOCNT || !(value & SIO_START) ||
        multiplayerChild() || multiplayerBusy_)
        return;
    if (!multiplayerReady()) {
        multiplayerError_ = true;
        return;
    }

    multiplayerWords_ = {read16Impl(REG_SIOMLT_SEND),
                         linkPeer_ ? linkPeer_->read16Impl(REG_SIOMLT_SEND)
                                   : uint16_t{0xFFFF}};
    const uint32_t baud = MULTIPLAYER_BAUD[value & 3];
    // Two 16-bit words with one start and one stop bit each.
    multiplayerCycles_ = (36 * CPU_CLOCK + baud - 1) / baud;
    remotePeerWordReady_ = !remoteLinkEnabled_;
    for (GbaSystem *endpoint : {this, linkPeer_}) {
        if (!endpoint)
            break;
        endpoint->multiplayerBusy_ = true;
        endpoint->multiplayerError_ = false;
        for (uint32_t slot = 0; slot < 4; ++slot)
            endpoint->storeIo16(REG_SIOMULTI0 + 2 * slot, 0xFFFF);
    }
    if (remoteLinkEnabled_) {
        markRemoteLinkActivity();
        if (!sendRemoteStart_)
            throw std::runtime_error("GBA remote link has no start sender");
        remoteActiveSequence_ = ++remoteTransferSequence_;
        sendRemoteStart_(remoteActiveSequence_, multiplayerWords_[0],
                         static_cast<uint8_t>(value & 3),
                         remoteElapsedCycles());
    }
}

void GbaSystem::abortMultiplayerTransfer() {
    const bool hadPendingWork = multiplayerBusy_ ||
        !remotePendingStarts_.empty();
    remotePendingStarts_.clear();
    remotePredictedWords_.clear();
    remotePendingWords_.clear();
    remotePeerWordReady_ = false;
    if (!multiplayerBusy_) {
        if (hadPendingWork)
            multiplayerError_ = true;
        return;
    }
    multiplayerBusy_ = false;
    multiplayerError_ = true;
    multiplayerCycles_ = 0;
    if (linkPeer_) {
        linkPeer_->multiplayerBusy_ = false;
        linkPeer_->multiplayerError_ = true;
        linkPeer_->multiplayerCycles_ = 0;
    }
}

void GbaSystem::completeMultiplayerTransfer() {
    multiplayerCycles_ = 0;
    if (linkPeer_) {
        for (GbaSystem *endpoint : {this, linkPeer_}) {
            endpoint->storeIo16(REG_SIOMULTI0,
                                multiplayerWords_[0]);
            endpoint->storeIo16(REG_SIOMULTI0 + 2,
                                multiplayerWords_[1]);
            endpoint->multiplayerBusy_ = false;
            if (endpoint->read16Impl(REG_SIOCNT) & SIO_IRQ_ENABLE)
                endpoint->storeIo16(REG_IF,
                    endpoint->read16Impl(REG_IF) | IRQ_SERIAL);
        }
    } else {
        for (uint32_t slot = 0; slot < 4; ++slot)
            storeIo16(REG_SIOMULTI0 + 2 * slot,
                      slot < 2 ? multiplayerWords_[slot] : 0xFFFF);
        multiplayerBusy_ = false;
        if (read16Impl(REG_SIOCNT) & SIO_IRQ_ENABLE)
            storeIo16(REG_IF, read16Impl(REG_IF) | IRQ_SERIAL);
    }
    remotePeerWordReady_ = false;
}

void GbaSystem::tickMultiplayer(uint32_t cycles) {
    if (!multiplayerBusy_ || multiplayerCycles_ == 0)
        return;
    if (cycles < multiplayerCycles_) {
        multiplayerCycles_ -= cycles;
        return;
    }
    multiplayerCycles_ = 0;
    if (remoteLinkEnabled_ && remoteLinkChild_ && !remoteReplySent_) {
        sendRemoteWord_(remoteActiveSequence_, multiplayerWords_[1]);
        remoteReplySent_ = true;
    }
    if (!remoteLinkEnabled_ || remotePeerWordReady_)
        completeMultiplayerTransfer();
}

void GbaSystem::tickNormalSerial(uint32_t cycles) {
    if (!normalSerialBusy_ || normalSerialCycles_ == 0)
        return;
    if (cycles < normalSerialCycles_) {
        normalSerialCycles_ -= cycles;
        return;
    }
    normalSerialCycles_ = 0;
    normalSerialBusy_ = false;
    const uint16_t control = read16Impl(REG_SIOCNT);
    storeIo16(REG_SIOCNT, control & ~SIO_START);
    if (control & 0x1000) {
        storeIo16(REG_SIOMULTI0, 0xFFFF);
        storeIo16(REG_SIOMULTI0 + 2, 0xFFFF);
    } else {
        storeIo16(REG_SIODATA8,
                  static_cast<uint16_t>((read16Impl(REG_SIODATA8) & 0xFF00) |
                                        0x00FF));
    }
    if (control & SIO_IRQ_ENABLE)
        storeIo16(REG_IF, read16Impl(REG_IF) | IRQ_SERIAL);
}

void GbaSystem::tick(uint32_t cycles) {
    rtc_.advanceCycles(cycles);
    tickMultiplayer(cycles);
    tickNormalSerial(cycles);
    audio_.advanceCycles(cycles);
    tickTimers(cycles);
    const auto requestInterrupt = [this](uint16_t interrupt) {
        const size_t offset = REG_IF - 0x04000000;
        const uint16_t flags = static_cast<uint16_t>(io_[offset] |
            (static_cast<uint16_t>(io_[offset + 1]) << 8));
        const uint16_t requested = static_cast<uint16_t>(flags | interrupt);
        io_[offset] = static_cast<uint8_t>(requested);
        io_[offset + 1] = static_cast<uint8_t>(requested >> 8);
    };

    uint32_t remaining = cycles;
    while (remaining != 0) {
        const uint16_t line = static_cast<uint16_t>(
            frameCycle_ / CYCLES_PER_SCANLINE);
        const uint16_t lineCycle = static_cast<uint16_t>(
            frameCycle_ % CYCLES_PER_SCANLINE);
        const uint32_t boundary = lineCycle < HDRAW_CYCLES
            ? HDRAW_CYCLES
            : CYCLES_PER_SCANLINE;
        const uint32_t step = std::min(remaining, boundary - lineCycle);
        frameCycle_ = (frameCycle_ + step) % CYCLES_PER_FRAME;
        remaining -= step;
        updateDisplayStatus();

        if (step != boundary - lineCycle)
            continue;

        if (boundary == 960) {
            if (line < SCREEN_HEIGHT) {
                if (renderedScanline_ != line) {
                    renderedScanline_ = line;
                    renderedPixel_ = 0;
                }
                if (renderedPixel_ < SCREEN_WIDTH) {
                    renderScanline(line, renderedPixel_, SCREEN_WIDTH);
                    renderedPixel_ = SCREEN_WIDTH;
                }
            }

            // Stage OAM for the incoming scanline before H-Blank DMA runs.
            renderedOam_ = oam_;
            renderedOamInitialized_ = true;

            if (line < SCREEN_HEIGHT) {
                for (unsigned channel = 0; channel < 4; ++channel)
                    triggerDma(channel, 2);
                if ((read16(REG_DISPSTAT) & (1u << 4)) != 0)
                    requestInterrupt(IRQ_HBLANK);
            }
            continue;
        }

        const uint16_t nextLine = static_cast<uint16_t>(
            (line + 1) % TOTAL_SCANLINES);
        if (nextLine == SCREEN_HEIGHT) {
            if ((read16(REG_DISPSTAT) & (1u << 3)) != 0)
                requestInterrupt(IRQ_VBLANK);
            for (unsigned channel = 0; channel < 4; ++channel)
                triggerDma(channel, 1);
        }
        if (nextLine == static_cast<uint8_t>(read16(REG_DISPSTAT) >> 8) &&
            (read16(REG_DISPSTAT) & (1u << 5)) != 0)
            requestInterrupt(IRQ_VCOUNT);
    }
}

void GbaSystem::flushCurrentScanline() {
    const uint16_t scanline = static_cast<uint16_t>(
        frameCycle_ / CYCLES_PER_SCANLINE);
    const uint32_t lineCycle = frameCycle_ % CYCLES_PER_SCANLINE;
    if (scanline >= SCREEN_HEIGHT || lineCycle >= HDRAW_CYCLES)
        return;

    if (renderedScanline_ != scanline) {
        renderedScanline_ = scanline;
        renderedPixel_ = 0;
    }

    const unsigned pixel = std::min<unsigned>(
        lineCycle / (HDRAW_CYCLES / SCREEN_WIDTH), SCREEN_WIDTH);
    if (pixel <= renderedPixel_)
        return;

    renderScanline(scanline, renderedPixel_, pixel);
    renderedPixel_ = pixel;
}

void GbaSystem::tickTimers(uint32_t cycles) {
    constexpr uint32_t PRESCALERS[] = {1, 64, 256, 1024};
    for (unsigned timer = 0; timer < 4; ++timer) {
        const uint16_t control = loadIo16(TIMER_BASE + timer * 4 + 2);
        if ((control & 0x0080) == 0 ||
            (timer != 0 && (control & 0x0004) != 0))
            continue;

        const uint32_t prescaler = PRESCALERS[control & 3];
        timerPrescalerCycles_[timer] += cycles;
        const uint32_t ticks = timerPrescalerCycles_[timer] / prescaler;
        timerPrescalerCycles_[timer] %= prescaler;
        if (ticks != 0)
            incrementTimer(timer, ticks);
    }
}

void GbaSystem::incrementTimer(unsigned timer, uint32_t ticks) {
    if (timer >= 4 || ticks == 0)
        return;

    const uint16_t control = loadIo16(TIMER_BASE + timer * 4 + 2);
    while (ticks != 0) {
        const uint32_t ticksUntilOverflow = 0x10000u - timerCounter_[timer];
        if (ticks < ticksUntilOverflow) {
            timerCounter_[timer] = static_cast<uint16_t>(
                timerCounter_[timer] + ticks);
            return;
        }

        ticks -= ticksUntilOverflow;
        timerCounter_[timer] = timerReload_[timer];
        const uint8_t refillMask = audio_.onTimerOverflow(timer);
        for (unsigned fifo = 0; fifo < 2; ++fifo) {
            if ((refillMask & (1u << fifo)) != 0)
                requestSoundDma(fifo);
        }
        if ((control & 0x0040) != 0) {
            const uint32_t interrupt = 1u << (3 + timer);
            const size_t offset = REG_IF - 0x04000000;
            const uint16_t flags = static_cast<uint16_t>(io_[offset] |
                (static_cast<uint16_t>(io_[offset + 1]) << 8));
            const uint16_t requested = static_cast<uint16_t>(flags | interrupt);
            io_[offset] = static_cast<uint8_t>(requested);
            io_[offset + 1] = static_cast<uint8_t>(requested >> 8);
        }

        if (timer < 3) {
            const uint16_t nextControl =
                loadIo16(TIMER_BASE + (timer + 1) * 4 + 2);
            if ((nextControl & 0x0084) == 0x0084)
                incrementTimer(timer + 1, 1);
        }
    }
}

void GbaSystem::requestSoundDma(unsigned fifo) {
    if (fifo >= 2)
        return;
    const uint32_t target = audio_.fifoAddress(fifo);
    for (unsigned channel = 1; channel <= 2; ++channel) {
        const uint32_t registerBase = 0x040000B0 + channel * 12;
        const uint16_t control = read16(registerBase + 10);
        if ((control & 0x8000) == 0 || ((control >> 12) & 3) != 3 ||
            (read32(registerBase + 4) & ~uint32_t{3}) != target)
            continue;
        triggerDma(channel, 3);
    }
}

void GbaSystem::triggerDma(unsigned channel, unsigned timing) {
    if (channel >= 4 || dmaActive_)
        return;

    const uint32_t registerBase = 0x040000B0 + channel * 12;
    const uint16_t control = read16(registerBase + 10);
    if ((control & 0x8000) == 0 || ((control >> 12) & 3) != timing)
        return;

    const uint32_t programmedDestination = read32(registerBase + 4) &
        ~uint32_t{3};
    const bool soundTransfer = timing == 3 && channel >= 1 && channel <= 2 &&
        (programmedDestination == audio_.fifoAddress(0) ||
         programmedDestination == audio_.fifoAddress(1));
    if (timing == 3 && !soundTransfer)
        return;

    const bool wordTransfer = soundTransfer || (control & (1u << 10)) != 0;
    const uint32_t unitSize = wordTransfer ? 4 : 2;
    uint32_t count = soundTransfer ? 4 : read16(registerBase + 8) &
        (channel == 0 ? 0x3FFFu : 0xFFFFu);
    if (count == 0)
        count = channel == 0 ? 0x4000 : 0x10000;

    if (!dmaInitialized_[channel]) {
        dmaSource_[channel] = read32(registerBase) & ~(unitSize - 1);
        dmaDestination_[channel] = read32(registerBase + 4) & ~(unitSize - 1);
        dmaInitialized_[channel] = true;
    }

    const uint32_t initialDestination = dmaDestination_[channel];
    uint32_t source = dmaSource_[channel];
    uint32_t destination = dmaDestination_[channel];
    const unsigned sourceMode = (control >> 7) & 3;
    const unsigned destinationMode = soundTransfer ? 2 : (control >> 5) & 3;
    const bool eepromWriteDma = saveType_ == SaveType::Eeprom &&
        !wordTransfer && isEepromAddress(destination);
    if (eepromWriteDma) {
        eepromInputBits_.clear();
        eepromDmaWriteActive_ = true;
    }
    dmaActive_ = true;
    for (uint32_t transfer = 0; transfer < count; ++transfer) {
        const uint32_t value = wordTransfer
            ? read32(source)
            : read16(source);
        if (wordTransfer)
            write32(destination, value);
        else
            write16(destination, static_cast<uint16_t>(value));

        if (sourceMode == 0 || sourceMode == 3)
            source += unitSize;
        else if (sourceMode == 1)
            source -= unitSize;

        if (destinationMode == 0 || destinationMode == 3)
            destination += unitSize;
        else if (destinationMode == 1)
            destination -= unitSize;
    }
    dmaActive_ = false;
    if (eepromWriteDma)
        finishEepromDma(count);

    dmaSource_[channel] = source;
    dmaDestination_[channel] = destinationMode == 3
        ? initialDestination
        : destination;

    const bool repeat = (control & (1u << 9)) != 0;
    if (timing == 0 || !repeat) {
        const uint32_t controlHighAddress = registerBase + 11;
        io_[controlHighAddress - 0x04000000] &= 0x7F;
        dmaInitialized_[channel] = false;
    }
}

void GbaSystem::updateDisplayStatus() {
    const uint16_t line = static_cast<uint16_t>(
        frameCycle_ / CYCLES_PER_SCANLINE);
    const uint16_t lineCycle = static_cast<uint16_t>(
        frameCycle_ % CYCLES_PER_SCANLINE);
    io_[REG_VCOUNT - 0x04000000] = static_cast<uint8_t>(line);
    io_[REG_VCOUNT - 0x04000000 + 1] = static_cast<uint8_t>(line >> 8);
    uint16_t status = static_cast<uint16_t>(io_[REG_DISPSTAT - 0x04000000] |
        (static_cast<uint16_t>(io_[REG_DISPSTAT - 0x04000000 + 1]) << 8));
    status &= static_cast<uint16_t>(~0x0007);
    if (line >= SCREEN_HEIGHT && line < TOTAL_SCANLINES - 1)
        status |= 1;
    if (lineCycle >= HDRAW_CYCLES)
        status |= 2;
    const uint8_t compare = io_[REG_DISPSTAT - 0x04000000 + 1];
    const bool matched = line == compare;
    if (matched)
        status |= 4;
    io_[REG_DISPSTAT - 0x04000000] = static_cast<uint8_t>(status);
    io_[REG_DISPSTAT - 0x04000000 + 1] = static_cast<uint8_t>(status >> 8);
}

void GbaSystem::offerPixel(LayerBuffer &pixels, size_t index, uint16_t color,
                           unsigned priority, unsigned layer,
                           unsigned order) {
    const auto inFront = [](const LayerPixel &first, const LayerPixel &second) {
        return !second.valid || first.priority < second.priority ||
            (first.priority == second.priority &&
             (first.layer < second.layer ||
              (first.layer == second.layer && first.order < second.order)));
    };
    const LayerPixel candidate{static_cast<uint16_t>(color & 0x7FFF),
        static_cast<uint16_t>(order), static_cast<uint8_t>(priority),
        static_cast<uint8_t>(layer), false, true};
    PixelLayers &current = pixels[index];
    if (inFront(candidate, current.top)) {
        current.second = current.top;
        current.top = candidate;
    } else if (inFront(candidate, current.second)) {
        current.second = candidate;
    }
}

void GbaSystem::offerObjectPixel(LayerPixel &pixel, uint16_t color,
                                 unsigned priority,
                                 unsigned object, bool semiTransparent) {
    const LayerPixel candidate{static_cast<uint16_t>(color & 0x7FFF),
        static_cast<uint16_t>(object), static_cast<uint8_t>(priority),
        0, semiTransparent, true};
    const auto inFront = [](const LayerPixel &first, const LayerPixel &second) {
        return !second.valid || first.priority < second.priority ||
            (first.priority == second.priority && first.order < second.order);
    };
    if (inFront(candidate, pixel))
        pixel = candidate;
}

void GbaSystem::renderTextBackground(unsigned background, unsigned y,
                                     LayerBuffer &pixels,
                                     const WindowBuffer &windows,
                                     unsigned firstPixel,
                                     unsigned endPixel) const {
    const uint16_t control = read16(0x04000008 + background * 2);
    const unsigned priority = control & 3;
    const uint32_t characterBase = ((control >> 2) & 3) * 0x4000;
    const bool eightBit = (control & 0x0080) != 0;
    const uint32_t screenBase = ((control >> 8) & 0x1F) * 0x800;
    const unsigned size = (control >> 14) & 3;
    const unsigned widthTiles = (size & 1) != 0 ? 64 : 32;
    const unsigned heightTiles = (size & 2) != 0 ? 64 : 32;
    const unsigned widthPixels = widthTiles * 8;
    const unsigned heightPixels = heightTiles * 8;
    const unsigned scrollX = read16(0x04000010 + background * 4) & 0x1FF;
    const unsigned scrollY = read16(0x04000012 + background * 4) & 0x1FF;
    const uint16_t mosaic = read16(0x0400004C);
    const bool mosaicEnabled = (control & (1u << 6)) != 0;
    const unsigned mosaicWidth = (mosaic & 0x0F) + 1;
    const unsigned mosaicHeight = ((mosaic >> 4) & 0x0F) + 1;

    const unsigned sampleY = mosaicEnabled ? y - y % mosaicHeight : y;
    const unsigned sourceY = (sampleY + scrollY) & (heightPixels - 1);
    const unsigned tileY = sourceY >> 3;
    const unsigned localY = sourceY & 7;
    for (unsigned x = firstPixel; x < endPixel; ++x) {
        if ((windows[x].layers & (1u << background)) == 0)
            continue;
        const unsigned sampleX = mosaicEnabled ? x - x % mosaicWidth : x;
        const unsigned sourceX = (sampleX + scrollX) & (widthPixels - 1);
        const unsigned tileX = sourceX >> 3;
        const unsigned blockX = tileX >> 5;
        const unsigned blockY = tileY >> 5;
        const unsigned blocksWide = widthTiles >> 5;
        const uint32_t blockOffset = (blockX + blockY * blocksWide) * 0x800;
        const uint32_t mapOffset = screenBase + blockOffset +
            ((tileY & 31) * 32 + (tileX & 31)) * 2;
        const uint16_t entry = read16(VRAM_BASE + mapOffset);
        const unsigned tile = entry & 0x03FF;
        unsigned localX = sourceX & 7;
        unsigned sampleTileY = localY;
        if ((entry & (1u << 10)) != 0)
            localX = 7 - localX;
        if ((entry & (1u << 11)) != 0)
            sampleTileY = 7 - sampleTileY;

        const uint32_t tileOffset = characterBase +
            tile * (eightBit ? 64u : 32u);
        unsigned colorIndex = 0;
        if (eightBit) {
            colorIndex = read8(VRAM_BASE + tileOffset + sampleTileY * 8 + localX);
        } else {
            const uint8_t packed = read8(VRAM_BASE + tileOffset +
                                          sampleTileY * 4 + localX / 2);
            colorIndex = (packed >> ((localX & 1) * 4)) & 0x0F;
        }
        if (colorIndex == 0)
            continue;

        const unsigned paletteIndex = eightBit
            ? colorIndex
            : (((entry >> 12) & 0x0F) * 16 + colorIndex);
        const uint16_t color = read16(PALETTE_BASE + paletteIndex * 2);
        offerPixel(pixels, x, color, priority, background + 1);
    }
}

void GbaSystem::renderAffineBackground(unsigned background, unsigned y,
                                       LayerBuffer &pixels,
                                       const WindowBuffer &windows,
                                       unsigned firstPixel,
                                       unsigned endPixel) const {
    const unsigned registerOffset = 0x20 + (background - 2) * 0x10;
    const uint16_t control = read16(0x04000008 + background * 2);
    const unsigned priority = control & 3;
    const uint32_t characterBase = ((control >> 2) & 3) * 0x4000;
    const uint32_t screenBase = ((control >> 8) & 0x1F) * 0x800;
    const unsigned size = (control >> 14) & 3;
    const unsigned sizeTiles = 16u << size;
    const unsigned sizePixels = sizeTiles * 8;
    const bool wrap = (control & (1u << 13)) != 0;
    const int32_t pa = static_cast<int16_t>(read16(0x04000000 + registerOffset));
    const int32_t pb = static_cast<int16_t>(read16(0x04000002 + registerOffset));
    const int32_t pc = static_cast<int16_t>(read16(0x04000004 + registerOffset));
    const int32_t pd = static_cast<int16_t>(read16(0x04000006 + registerOffset));
    const int64_t referenceX = signExtend28(read32(0x04000008 + registerOffset));
    const int64_t referenceY = signExtend28(read32(0x0400000C + registerOffset));
    const uint16_t mosaic = read16(0x0400004C);
    const bool mosaicEnabled = (control & (1u << 6)) != 0;
    const unsigned mosaicWidth = (mosaic & 0x0F) + 1;
    const unsigned mosaicHeight = ((mosaic >> 4) & 0x0F) + 1;

    const unsigned sampleY = mosaicEnabled ? y - y % mosaicHeight : y;
    for (unsigned x = firstPixel; x < endPixel; ++x) {
        if ((windows[x].layers & (1u << background)) == 0)
            continue;
        const unsigned sampleX = mosaicEnabled ? x - x % mosaicWidth : x;
        int64_t sourceX = fixedToInteger(referenceX +
            static_cast<int64_t>(pa) * sampleX +
            static_cast<int64_t>(pb) * sampleY);
        int64_t sourceY = fixedToInteger(referenceY +
            static_cast<int64_t>(pc) * sampleX +
            static_cast<int64_t>(pd) * sampleY);
        if (wrap) {
            sourceX &= sizePixels - 1;
            sourceY &= sizePixels - 1;
        } else if (sourceX < 0 || sourceY < 0 ||
                   sourceX >= sizePixels || sourceY >= sizePixels) {
            continue;
        }

        const unsigned pixelX = static_cast<unsigned>(sourceX);
        const unsigned pixelY = static_cast<unsigned>(sourceY);
        const unsigned tileX = pixelX >> 3;
        const unsigned tileY = pixelY >> 3;
        const uint8_t tile = read8(VRAM_BASE + screenBase +
                                   tileY * sizeTiles + tileX);
        const unsigned colorIndex = read8(VRAM_BASE + characterBase +
            static_cast<uint32_t>(tile) * 64 + (pixelY & 7) * 8 + (pixelX & 7));
        if (colorIndex == 0)
            continue;
        const uint16_t color = read16(PALETTE_BASE + colorIndex * 2);
        offerPixel(pixels, x, color, priority, background + 1);
    }
}

void GbaSystem::renderBitmapBackground(unsigned mode, unsigned y,
                                       uint16_t display,
                                       LayerBuffer &pixels,
                                       const WindowBuffer &windows,
                                       unsigned firstPixel,
                                       unsigned endPixel) const {
    if ((display & (1u << 10)) == 0)
        return;

    const unsigned priority = read16(0x0400000C) & 3;
    const unsigned width = mode == 5 ? 160 : SCREEN_WIDTH;
    const unsigned height = mode == 5 ? 128 : SCREEN_HEIGHT;
    const uint32_t page = mode >= 4 && (display & (1u << 4)) != 0
        ? 0xA000
        : 0;
    const int32_t pa = static_cast<int16_t>(read16(0x04000020));
    const int32_t pb = static_cast<int16_t>(read16(0x04000022));
    const int32_t pc = static_cast<int16_t>(read16(0x04000024));
    const int32_t pd = static_cast<int16_t>(read16(0x04000026));
    const int64_t referenceX = signExtend28(read32(0x04000028));
    const int64_t referenceY = signExtend28(read32(0x0400002C));
    const uint16_t control = read16(0x0400000C);
    const uint16_t mosaic = read16(0x0400004C);
    const bool mosaicEnabled = (control & (1u << 6)) != 0;
    const unsigned mosaicWidth = (mosaic & 0x0F) + 1;
    const unsigned mosaicHeight = ((mosaic >> 4) & 0x0F) + 1;

    const unsigned sampleY = mosaicEnabled ? y - y % mosaicHeight : y;
    for (unsigned x = firstPixel; x < endPixel; ++x) {
        if ((windows[x].layers & (1u << 2)) == 0)
            continue;
        const unsigned sampleX = mosaicEnabled ? x - x % mosaicWidth : x;
        const int64_t sourceX = fixedToInteger(referenceX +
            static_cast<int64_t>(pa) * sampleX +
            static_cast<int64_t>(pb) * sampleY);
        const int64_t sourceY = fixedToInteger(referenceY +
            static_cast<int64_t>(pc) * sampleX +
            static_cast<int64_t>(pd) * sampleY);
        if (sourceX < 0 || sourceY < 0 ||
            sourceX >= width || sourceY >= height)
            continue;

        const uint32_t sourceIndex = static_cast<uint32_t>(sourceY) * width +
                                     static_cast<uint32_t>(sourceX);
        uint16_t color = 0;
        if (mode == 4) {
            const unsigned paletteIndex = read8(VRAM_BASE + page + sourceIndex);
            if (paletteIndex == 0)
                continue;
            color = read16(PALETTE_BASE + paletteIndex * 2);
        } else {
            color = read16(VRAM_BASE + page + sourceIndex * 2);
        }
        offerPixel(pixels, x, color, priority, 3);
    }
}

GbaSystem::ObjectVisibility GbaSystem::selectObjectsForScanline(
    unsigned scanline, uint16_t display) const {
    ObjectVisibility visible{};
    if ((display & (1u << 12)) == 0)
        return visible;

    unsigned cyclesUsed = 0;
    const unsigned cycleLimit = (display & (1u << 5)) != 0 ? 954 : 1210;
    constexpr unsigned OAM_SCAN_CYCLES = 2;
    for (unsigned object = 0; object < 128; ++object) {
        if (cyclesUsed + OAM_SCAN_CYCLES > cycleLimit)
            break;
        cyclesUsed += OAM_SCAN_CYCLES;

        const uint32_t objectAddress = OAM_BASE + object * 8;
        const uint16_t attribute0 = readOamForRendering(objectAddress);
        const uint16_t attribute1 = readOamForRendering(objectAddress + 2);
        const bool affine = (attribute0 & (1u << 8)) != 0;
        const bool disabled = !affine && (attribute0 & (1u << 9)) != 0;
        const unsigned objectMode = (attribute0 >> 10) & 3;
        const unsigned shape = (attribute0 >> 14) & 3;
        if (disabled || shape >= 3 || objectMode == 3 ||
            (objectMode == 2 && (display & (1u << 15)) == 0))
            continue;

        const unsigned size = (attribute1 >> 14) & 3;
        const unsigned width = OBJ_DIMENSIONS[shape][size][0];
        const unsigned height = OBJ_DIMENSIONS[shape][size][1];
        const bool doubleSize = affine && (attribute0 & (1u << 9)) != 0;
        const unsigned boundsWidth = doubleSize ? width * 2 : width;
        const unsigned boundsHeight = doubleSize ? height * 2 : height;
        int x = attribute1 & 0x01FF;
        int y = attribute0 & 0x00FF;
        if (x >= 256)
            x -= 512;
        if (y >= static_cast<int>(SCREEN_HEIGHT))
            y -= 256;
        const bool intersectsScanline = static_cast<int>(scanline) >= y &&
            static_cast<int>(scanline) < y + static_cast<int>(boundsHeight);
        const bool intersectsScreen = x < static_cast<int>(SCREEN_WIDTH) &&
            x + static_cast<int>(boundsWidth) > 0;
        if (!intersectsScanline || !intersectsScreen)
            continue;

        int renderCycles = affine
            ? static_cast<int>(8 + boundsWidth * 2)
            : static_cast<int>(width) - 2;
        if (x < 0) {
            // The renderer avoids fetching pixels clipped at the left edge.
            // Normal OBJ cost drops once per two clipped pixels; affine OBJ
            // cost drops once per clipped pixel.
            renderCycles += affine ? x : -((-x + 1) / 2);
        }
        const unsigned spriteCycles = static_cast<unsigned>(renderCycles);
        if (cyclesUsed + spriteCycles > cycleLimit)
            break;
        cyclesUsed += spriteCycles;
        visible[object] = true;
    }
    return visible;
}

void GbaSystem::renderObjectWindow(unsigned scanline, bool oneDimensional,
                                   bool bitmapMode,
                                   const ObjectVisibility &objectVisibility,
                                   ObjectWindowBuffer &objectWindow,
                                   unsigned firstPixel,
                                   unsigned endPixel) const {
    const uint32_t characterBase = VRAM_BASE + (bitmapMode ? 0x14000 : 0x10000);
    const uint16_t mosaic = read16(0x0400004C);
    const unsigned mosaicWidth = ((mosaic >> 8) & 0x0F) + 1;
    const unsigned mosaicHeight = ((mosaic >> 12) & 0x0F) + 1;

    for (unsigned object = 0; object < 128; ++object) {
        if (!objectVisibility[object])
            continue;
        const uint32_t objectAddress = OAM_BASE + object * 8;
        const uint16_t attribute0 = readOamForRendering(objectAddress);
        const uint16_t attribute1 = readOamForRendering(objectAddress + 2);
        const uint16_t attribute2 = readOamForRendering(objectAddress + 4);
        const bool affine = (attribute0 & (1u << 8)) != 0;
        if (((attribute0 >> 10) & 3) != 2 || (!affine && (attribute0 & 0x0200) != 0))
            continue;
        const unsigned shape = (attribute0 >> 14) & 3;
        if (shape >= 3)
            continue;

        const unsigned size = (attribute1 >> 14) & 3;
        const unsigned width = OBJ_DIMENSIONS[shape][size][0];
        const unsigned height = OBJ_DIMENSIONS[shape][size][1];
        const bool doubleSize = affine && (attribute0 & (1u << 9)) != 0;
        const unsigned boundsWidth = doubleSize ? width * 2 : width;
        const unsigned boundsHeight = doubleSize ? height * 2 : height;
        int x = attribute1 & 0x01FF;
        int y = attribute0 & 0x00FF;
        if (x >= 256)
            x -= 512;
        if (y >= static_cast<int>(SCREEN_HEIGHT))
            y -= 256;

        const int left = std::max({0, x, static_cast<int>(firstPixel)});
        const int top = std::max(0, y);
        const int right = std::min(static_cast<int>(SCREEN_WIDTH),
            std::min(static_cast<int>(endPixel),
                     x + static_cast<int>(boundsWidth)));
        const int bottom = std::min(static_cast<int>(SCREEN_HEIGHT),
                                    y + static_cast<int>(boundsHeight));
        if (left >= right || static_cast<int>(scanline) < top ||
            static_cast<int>(scanline) >= bottom)
            continue;

        const bool eightBit = (attribute0 & (1u << 13)) != 0;
        const bool mosaicEnabled = (attribute0 & (1u << 12)) != 0;
        unsigned baseTile = 0;
        if (!decodeObjectBaseTile(attribute2, eightBit, bitmapMode, baseTile))
            continue;
        int16_t pa = 0x0100;
        int16_t pb = 0;
        int16_t pc = 0;
        int16_t pd = 0x0100;
        if (affine) {
            const unsigned matrix = (attribute1 >> 9) & 0x1F;
            const uint32_t matrixAddress = OAM_BASE + matrix * 32;
            pa = static_cast<int16_t>(readOamForRendering(matrixAddress + 6));
            pb = static_cast<int16_t>(readOamForRendering(matrixAddress + 14));
            pc = static_cast<int16_t>(readOamForRendering(matrixAddress + 22));
            pd = static_cast<int16_t>(readOamForRendering(matrixAddress + 30));
        }

        const int screenY = static_cast<int>(scanline);
        const int sampleScreenY = mosaicEnabled
            ? std::max(y, screenY - screenY % static_cast<int>(mosaicHeight))
            : screenY;
        for (int screenX = left; screenX < right; ++screenX) {
            const int sampleScreenX = mosaicEnabled
                ? std::max(x, screenX - screenX % static_cast<int>(mosaicWidth))
                : screenX;
            unsigned sourceX = 0;
            unsigned sourceY = 0;
            if (affine) {
                const int64_t dx = sampleScreenX -
                    (x + static_cast<int>(boundsWidth / 2));
                const int64_t dy = sampleScreenY -
                    (y + static_cast<int>(boundsHeight / 2));
                const int64_t sourceXFixed = static_cast<int64_t>(pa) * dx +
                    static_cast<int64_t>(pb) * dy +
                    static_cast<int64_t>(width / 2) * 256;
                const int64_t sourceYFixed = static_cast<int64_t>(pc) * dx +
                    static_cast<int64_t>(pd) * dy +
                    static_cast<int64_t>(height / 2) * 256;
                const int64_t affineX = fixedToInteger(sourceXFixed);
                const int64_t affineY = fixedToInteger(sourceYFixed);
                if (affineX < 0 || affineY < 0 ||
                    affineX >= width || affineY >= height)
                    continue;
                sourceX = static_cast<unsigned>(affineX);
                sourceY = static_cast<unsigned>(affineY);
            } else {
                sourceX = static_cast<unsigned>(sampleScreenX - x);
                sourceY = static_cast<unsigned>(sampleScreenY - y);
                if ((attribute1 & (1u << 12)) != 0)
                    sourceX = width - 1 - sourceX;
                if ((attribute1 & (1u << 13)) != 0)
                    sourceY = height - 1 - sourceY;
            }

            const unsigned tilesAcross = width / 8;
            const unsigned tileX = sourceX / 8;
            const unsigned tileY = sourceY / 8;
            const unsigned tileNumber = oneDimensional
                ? baseTile + (tileY * tilesAcross + tileX) * (eightBit ? 2 : 1)
                : baseTile + tileY * 32 + tileX * (eightBit ? 2 : 1);
            const uint32_t tileAddress = characterBase + tileNumber * 32;
            unsigned colorIndex = 0;
            if (eightBit) {
                colorIndex = read8(tileAddress + (sourceY & 7) * 8 +
                                   (sourceX & 7));
            } else {
                const uint8_t packed = read8(tileAddress + (sourceY & 7) * 4 +
                                             (sourceX & 7) / 2);
                colorIndex = (packed >> ((sourceX & 1) * 4)) & 0x0F;
            }
            if (colorIndex != 0)
                objectWindow[static_cast<unsigned>(screenX)] = 1;
        }
    }
}

void GbaSystem::renderObjects(unsigned scanline, bool oneDimensional,
                              bool bitmapMode,
                              const ObjectVisibility &objectVisibility,
                              ObjectBuffer &objects,
                              unsigned firstPixel,
                              unsigned endPixel) const {
    const uint32_t characterBase = VRAM_BASE + (bitmapMode ? 0x14000 : 0x10000);
    const uint16_t mosaic = read16(0x0400004C);
    const unsigned mosaicWidth = ((mosaic >> 8) & 0x0F) + 1;
    const unsigned mosaicHeight = ((mosaic >> 12) & 0x0F) + 1;
    for (unsigned object = 0; object < 128; ++object) {
        if (!objectVisibility[object])
            continue;
        const uint32_t objectAddress = OAM_BASE + object * 8;
        const uint16_t attribute0 = readOamForRendering(objectAddress);
        const uint16_t attribute1 = readOamForRendering(objectAddress + 2);
        const uint16_t attribute2 = readOamForRendering(objectAddress + 4);
        const bool affine = (attribute0 & (1u << 8)) != 0;
        const bool disabled = !affine && (attribute0 & (1u << 9)) != 0;
        const unsigned objectMode = (attribute0 >> 10) & 3;
        const unsigned shape = (attribute0 >> 14) & 3;
        if (disabled || objectMode == 3 || shape >= 3)
            continue;

        const unsigned size = (attribute1 >> 14) & 3;
        const unsigned width = OBJ_DIMENSIONS[shape][size][0];
        const unsigned height = OBJ_DIMENSIONS[shape][size][1];
        const bool doubleSize = affine && (attribute0 & (1u << 9)) != 0;
        const unsigned boundsWidth = doubleSize ? width * 2 : width;
        const unsigned boundsHeight = doubleSize ? height * 2 : height;
        int x = attribute1 & 0x01FF;
        int y = attribute0 & 0x00FF;
        if (x >= 256)
            x -= 512;
        if (y >= static_cast<int>(SCREEN_HEIGHT))
            y -= 256;

        const int left = std::max({0, x, static_cast<int>(firstPixel)});
        const int top = std::max(0, y);
        const int right = std::min(static_cast<int>(SCREEN_WIDTH),
            std::min(static_cast<int>(endPixel),
                     x + static_cast<int>(boundsWidth)));
        const int bottom = std::min(static_cast<int>(SCREEN_HEIGHT),
                                    y + static_cast<int>(boundsHeight));
        if (left >= right || static_cast<int>(scanline) < top ||
            static_cast<int>(scanline) >= bottom)
            continue;

        const bool eightBit = (attribute0 & (1u << 13)) != 0;
        const bool mosaicEnabled = (attribute0 & (1u << 12)) != 0;
        unsigned baseTile = 0;
        if (!decodeObjectBaseTile(attribute2, eightBit, bitmapMode, baseTile))
            continue;
        const unsigned paletteBank = (attribute2 >> 12) & 0x0F;
        const unsigned priority = (attribute2 >> 10) & 3;
        int16_t pa = 0x0100;
        int16_t pb = 0;
        int16_t pc = 0;
        int16_t pd = 0x0100;
        if (affine) {
            const unsigned matrix = (attribute1 >> 9) & 0x1F;
            const uint32_t matrixAddress = OAM_BASE + matrix * 32;
            pa = static_cast<int16_t>(readOamForRendering(matrixAddress + 6));
            pb = static_cast<int16_t>(readOamForRendering(matrixAddress + 14));
            pc = static_cast<int16_t>(readOamForRendering(matrixAddress + 22));
            pd = static_cast<int16_t>(readOamForRendering(matrixAddress + 30));
        }

        const int screenY = static_cast<int>(scanline);
        const int sampleScreenY = mosaicEnabled
            ? std::max(y, screenY - screenY % static_cast<int>(mosaicHeight))
            : screenY;
        for (int screenX = left; screenX < right; ++screenX) {
            const unsigned pixelIndex = static_cast<unsigned>(screenX);
            const int sampleScreenX = mosaicEnabled
                ? std::max(x, screenX - screenX % static_cast<int>(mosaicWidth))
                : screenX;
            unsigned sourceX = 0;
            unsigned sourceY = 0;
            if (affine) {
                const int64_t dx = sampleScreenX -
                    (x + static_cast<int>(boundsWidth / 2));
                const int64_t dy = sampleScreenY -
                    (y + static_cast<int>(boundsHeight / 2));
                const int64_t sourceXFixed = static_cast<int64_t>(pa) * dx +
                    static_cast<int64_t>(pb) * dy +
                    static_cast<int64_t>(width / 2) * 256;
                const int64_t sourceYFixed = static_cast<int64_t>(pc) * dx +
                    static_cast<int64_t>(pd) * dy +
                    static_cast<int64_t>(height / 2) * 256;
                const int64_t affineX = fixedToInteger(sourceXFixed);
                const int64_t affineY = fixedToInteger(sourceYFixed);
                if (affineX < 0 || affineY < 0 || affineX >= width || affineY >= height)
                    continue;
                sourceX = static_cast<unsigned>(affineX);
                sourceY = static_cast<unsigned>(affineY);
            } else {
                sourceX = static_cast<unsigned>(sampleScreenX - x);
                sourceY = static_cast<unsigned>(sampleScreenY - y);
                if ((attribute1 & (1u << 12)) != 0)
                    sourceX = width - 1 - sourceX;
                if ((attribute1 & (1u << 13)) != 0)
                    sourceY = height - 1 - sourceY;
            }

            const unsigned tilesAcross = width / 8;
            const unsigned tileX = sourceX / 8;
            const unsigned tileY = sourceY / 8;
            const unsigned tileNumber = oneDimensional
                ? baseTile + (tileY * tilesAcross + tileX) * (eightBit ? 2 : 1)
                : baseTile + tileY * 32 + tileX * (eightBit ? 2 : 1);
            const uint32_t tileAddress = characterBase + tileNumber * 32;
            unsigned colorIndex = 0;
            if (eightBit) {
                colorIndex = read8(tileAddress + (sourceY & 7) * 8 + (sourceX & 7));
            } else {
                const uint8_t packed = read8(tileAddress + (sourceY & 7) * 4 +
                                             (sourceX & 7) / 2);
                colorIndex = (packed >> ((sourceX & 1) * 4)) & 0x0F;
            }
            LayerPixel &currentObjectPixel = objects[pixelIndex];
            if (objectMode == 2) {
                if (colorIndex == 0 && currentObjectPixel.valid &&
                    priority < currentObjectPixel.priority) {
                    currentObjectPixel.priority =
                        static_cast<uint8_t>(priority);
                }
                continue;
            }
            if (colorIndex == 0) {
                // Transparent pixels leave the color intact but can promote
                // an earlier OBJ pixel to this object's BG priority.
                if (currentObjectPixel.valid &&
                    priority < currentObjectPixel.priority) {
                    currentObjectPixel.priority =
                        static_cast<uint8_t>(priority);
                }
                continue;
            }

            const unsigned paletteIndex = eightBit
                ? colorIndex
                : paletteBank * 16 + colorIndex;
            const uint16_t color = read16(OBJ_PALETTE_BASE + paletteIndex * 2);
        offerObjectPixel(objects[pixelIndex], color, priority, object,
                             objectMode == 1);
        }
    }
}

void GbaSystem::renderScanline(unsigned y, unsigned firstPixel,
                               unsigned endPixel) {
    const size_t rowOffset = y * SCREEN_WIDTH;
    const uint16_t display = read16(REG_DISPCNT);
    const ObjectVisibility objectVisibility =
        selectObjectsForScanline(y, display);
    if ((display & (1u << 7)) != 0) {
        std::fill(frameBuffer_.begin() + static_cast<std::ptrdiff_t>(
                      rowOffset + firstPixel),
                  frameBuffer_.begin() + static_cast<std::ptrdiff_t>(
                      rowOffset + endPixel),
                  0x7FFF);
        return;
    }

    ObjectWindowBuffer objectWindow{};
    if ((display & ((1u << 12) | (1u << 15))) ==
        ((1u << 12) | (1u << 15))) {
        renderObjectWindow(y, (display & (1u << 6)) != 0,
                           (display & 7) >= 3,
                           objectVisibility,
                           objectWindow, firstPixel, endPixel);
    }

    WindowBuffer windows{};
    if ((display & 0xE000) == 0) {
        windows.fill({0x3F, true});
    } else {
        const uint16_t win0h = read16(0x04000040);
        const uint16_t win1h = read16(0x04000042);
        const uint16_t win0v = read16(0x04000044);
        const uint16_t win1v = read16(0x04000046);
        const uint16_t winin = read16(0x04000048);
        const uint16_t winout = read16(0x0400004A);
        const auto horizontalContains = [](unsigned start, unsigned end,
                                           unsigned x) {
            if (start > SCREEN_WIDTH && start > end)
                start = 0;
            if (end > SCREEN_WIDTH) {
                end = SCREEN_WIDTH;
                if (start > SCREEN_WIDTH)
                    start = SCREEN_WIDTH;
            }
            if (start == end)
                return false;
            if (start < end)
                return x >= start && x < end;
            return x >= start || x < end;
        };
        const auto verticalContains = [](unsigned start, unsigned end,
                                         unsigned y) {
            if (start == end)
                return false;
            if (start < end)
                return y >= start && y < end;
            // VCOUNT runs through the VBlank lines before returning to zero,
            // so a start in VBlank can wrap into the next visible frame.
            return start < TOTAL_SCANLINES && (y >= start || y < end);
        };
        const auto contains = [&](uint16_t horizontal, uint16_t vertical,
                                  unsigned x, unsigned y) {
            return horizontalContains(horizontal >> 8, horizontal & 0xFF, x) &&
                   verticalContains(vertical >> 8, vertical & 0xFF, y);
        };

        for (unsigned x = firstPixel; x < endPixel; ++x) {
            uint8_t layerMask = static_cast<uint8_t>(winout & 0x3F);
            bool colorEffects = (winout & (1u << 5)) != 0;
            if ((display & (1u << 13)) != 0 &&
                contains(win0h, win0v, x, y)) {
                layerMask = static_cast<uint8_t>(winin & 0x3F);
                colorEffects = (winin & (1u << 5)) != 0;
            } else if ((display & (1u << 14)) != 0 &&
                       contains(win1h, win1v, x, y)) {
                layerMask = static_cast<uint8_t>((winin >> 8) & 0x3F);
                colorEffects = (winin & (1u << 13)) != 0;
            } else if ((display & (1u << 15)) != 0 &&
                       objectWindow[x] != 0) {
                layerMask = static_cast<uint8_t>((winout >> 8) & 0x3F);
                colorEffects = (winout & (1u << 13)) != 0;
            }
            windows[x] = {layerMask, colorEffects};
        }
    }

    const unsigned mode = display & 7;
    const uint16_t backdrop = read16(PALETTE_BASE) & 0x7FFF;
    LayerBuffer pixels{};
    ObjectBuffer objects{};
    for (PixelLayers &layers : pixels)
        layers.top = {backdrop, 0xFFFF, 4, 5, false, true};

    if (mode == 0) {
        for (unsigned background = 0; background < 4; ++background) {
            if ((display & (1u << (8 + background))) != 0)
                renderTextBackground(background, y, pixels, windows,
                                     firstPixel, endPixel);
        }
    } else if (mode == 1) {
        for (unsigned background = 0; background < 2; ++background) {
            if ((display & (1u << (8 + background))) != 0)
                renderTextBackground(background, y, pixels, windows,
                                     firstPixel, endPixel);
        }
        if ((display & (1u << 10)) != 0)
            renderAffineBackground(2, y, pixels, windows,
                                   firstPixel, endPixel);
    } else if (mode == 2) {
        for (unsigned background = 2; background < 4; ++background) {
            if ((display & (1u << (8 + background))) != 0)
                renderAffineBackground(background, y, pixels, windows,
                                       firstPixel, endPixel);
        }
    } else if (mode >= 3 && mode <= 5) {
        renderBitmapBackground(mode, y, display, pixels, windows,
                               firstPixel, endPixel);
    }

    if ((display & (1u << 12)) != 0 && mode <= 5)
        renderObjects(y, (display & (1u << 6)) != 0, mode >= 3,
                      objectVisibility, objects, firstPixel, endPixel);

    const uint16_t blendControl = read16(0x04000050);
    const uint16_t blendAlpha = read16(0x04000052);
    const unsigned blendMode = (blendControl >> 6) & 3;
    const uint8_t firstTargets = static_cast<uint8_t>(blendControl & 0x3F);
    const uint8_t secondTargets = static_cast<uint8_t>((blendControl >> 8) & 0x3F);
    const unsigned eva = std::min(16u, static_cast<unsigned>(blendAlpha & 0x1F));
    const unsigned evb = std::min(16u,
        static_cast<unsigned>((blendAlpha >> 8) & 0x1F));
    const unsigned evy = std::min(16u,
        static_cast<unsigned>(read16(0x04000054) & 0x1F));
    const auto targetBit = [](unsigned layer) {
        if (layer == 0)
            return 1u << 4;
        if (layer < 5)
            return 1u << (layer - 1);
        return 1u << 5;
    };
    const auto isInFront = [](const LayerPixel &first, const LayerPixel &second) {
        return first.priority < second.priority ||
            (first.priority == second.priority &&
             (first.layer < second.layer ||
              (first.layer == second.layer && first.order < second.order)));
    };
    const auto offerLayer = [&isInFront](PixelLayers &composite,
                                         const LayerPixel &layer) {
        if (!layer.valid)
            return;
        if (isInFront(layer, composite.top)) {
            composite.second = composite.top;
            composite.top = layer;
        } else if (!composite.second.valid ||
                   isInFront(layer, composite.second)) {
            composite.second = layer;
        }
    };
    const auto blend = [](uint16_t first, uint16_t second,
                          unsigned weightFirst, unsigned weightSecond) {
        const auto channel = [weightFirst, weightSecond](unsigned a, unsigned b) {
            return std::min(31u, (a * weightFirst + b * weightSecond) >> 4);
        };
        const unsigned red = channel(first & 0x1F, second & 0x1F);
        const unsigned green = channel((first >> 5) & 0x1F,
                                       (second >> 5) & 0x1F);
        const unsigned blue = channel((first >> 10) & 0x1F,
                                      (second >> 10) & 0x1F);
        return static_cast<uint16_t>(red | (green << 5) | (blue << 10));
    };
    const auto adjustBrightness = [evy](uint16_t color, bool brighten) {
        const auto channel = [evy, brighten](unsigned value) {
            return brighten
                ? value + (((31u - value) * evy) >> 4)
                : value - ((value * evy) >> 4);
        };
        const unsigned red = channel(color & 0x1F);
        const unsigned green = channel((color >> 5) & 0x1F);
        const unsigned blue = channel((color >> 10) & 0x1F);
        return static_cast<uint16_t>(red | (green << 5) | (blue << 10));
    };

    for (unsigned x = firstPixel; x < endPixel; ++x) {
        PixelLayers composite = pixels[x];
        if ((windows[x].layers & (1u << 4)) != 0)
            offerLayer(composite, objects[x]);

        const LayerPixel &top = composite.top;
        uint16_t color = top.color;
        if (windows[x].colorEffects) {
            if (top.semiTransparent) {
                if (composite.second.valid &&
                    (secondTargets & targetBit(composite.second.layer)) != 0) {
                    color = blend(top.color, composite.second.color, eva, evb);
                }
            } else if ((firstTargets & targetBit(top.layer)) != 0) {
                if (blendMode == 1 && composite.second.valid &&
                    (secondTargets & targetBit(composite.second.layer)) != 0) {
                    color = blend(top.color, composite.second.color, eva, evb);
                } else if (blendMode == 2) {
                    color = adjustBrightness(top.color, true);
                } else if (blendMode == 3) {
                    color = adjustBrightness(top.color, false);
                }
            }
        }

        frameBuffer_[rowOffset + x] = color;
    }
}

void GbaSystem::initializeScanlineOam() {
    if (!renderedOamInitialized_) {
        renderedOam_ = oam_;
        renderedOamInitialized_ = true;
    }
}

Arm7Tdmi::StepResult GbaSystem::stepCpu() {
    // These internal checks read the stored registers without a CPU bus access.
    const uint16_t flags = loadIo16(REG_IF) & 0x3FFF;
    const uint16_t enable = loadIo16(REG_IE) & 0x3FFF;
    cpu_.wakeFromInterrupts(flags, enable);
    if ((io_[REG_IME - 0x04000000] & 1) != 0 && (flags & enable) != 0 &&
        !cpu_.irqDisabled()) {
        if (bios_.empty()) {
            const uint32_t handler = read32(0x03007FFC);
            cpu_.enterIrq(handler != 0 ? handler : 0x00000018);
        } else {
            cpu_.enterIrqVector();
        }
    }
    const Arm7Tdmi::StepResult result = cpu_.step(*this);
    if (result.success) {
        tick(result.cycles);
        emulatedCycles_ += result.cycles;
    }
    return result;
}

GbaSystem::FrameResult GbaSystem::runFrame() {
    initializeScanlineOam();
    uint32_t elapsed = 0;
    while (remoteFrameCycles_ < CYCLES_PER_FRAME) {
        beginRemoteMultiplayerTransfer();
        applyPendingRemoteWord();

        // Do not let guest code run past a completed local SIO clock while
        // the remote word is still in transit unless the frame-credit window
        // allows bounded guest progress. Keep SIOCNT busy and never fabricate
        // a receive word or completion IRQ.
        if (remoteLinkEnabled_ && multiplayerBusy_ &&
            multiplayerCycles_ == 0 && !remotePeerWordReady_) {
            if (remoteEventPump_)
                remoteEventPump_();
        }
        if (remoteLinkEnabled_ && multiplayerBusy_ &&
            multiplayerCycles_ == 0 && !remotePeerWordReady_) {
            if (predictRemoteWord_) {
                const auto predicted = predictRemoteWord_(
                    remoteActiveSequence_, multiplayerWords_[0]);
                if (predicted) {
                    // Optional compatibility hook for a narrowly verified
                    // deterministic reply. A late real word must match this
                    // value or the transfer reports an error.
                    remotePredictedWords_[remoteActiveSequence_] = *predicted;
                    multiplayerWords_[1] = *predicted;
                    remotePeerWordReady_ = true;
                    remotePredictionUsed_ = true;
                    completeMultiplayerTransfer();
                    markRemoteLinkActivity();
                }
            }
        }
        if (remoteLinkEnabled_ && multiplayerBusy_ &&
            multiplayerCycles_ == 0 && !remotePeerWordReady_) {
            if (remoteEventPump_) {
                // Do not run guest code or VBlank timers past a transfer that
                // is waiting for its real peer word. The frontend can present
                // the current frame again while polling the network on its
                // next run; the guest resumes as soon as the word arrives.
                remoteFrameStalled_ = true;
                return {true, elapsed};
            }
            if (shouldPaceRemoteFrame()) {
                remoteFrameStalled_ = true;
                return {true, elapsed};
            }
        }
        if (shouldPaceRemoteFrame()) {
            remoteFrameStalled_ = true;
            return {true, elapsed};
        }
        remoteFrameStalled_ = false;
        const auto result = stepCpu();
        if (!result.success)
            return {false, elapsed};
        remoteFrameCycles_ += result.cycles;
        elapsed += result.cycles;
    }
    remoteFrameCycles_ = 0;
    remoteFrameStalled_ = false;
    remoteInputLatch_ = 0;
    if (remoteLinkEnabled_) {
        ++remoteFrameCount_;
        if (remoteLinkConnected_ && sendRemoteFrame_ && remoteLocalReady_)
            sendRemoteFrame_(remoteFrameCount_);
    }
    return {true, elapsed};
}

GbaSystem::FrameResult GbaSystem::runLinkedFrame() {
    if (!linkPeer_ || linkChild_)
        throw std::logic_error("Only the parent can run a linked GBA frame");
    initializeScanlineOam();
    linkPeer_->initializeScanlineOam();
    uint32_t parentTime = linkedFrameCarry_;
    uint32_t childTime = linkPeer_->linkedFrameCarry_;
    while (std::min(parentTime, childTime) < CYCLES_PER_FRAME) {
        const bool runParent = parentTime <= childTime;
        auto &endpoint = runParent ? *this : *linkPeer_;
        const auto result = endpoint.stepCpu();
        if (!result.success)
            return {false, std::min(parentTime, childTime)};
        (runParent ? parentTime : childTime) += result.cycles;
    }
    const uint32_t elapsed = parentTime - linkedFrameCarry_;
    linkedFrameCarry_ = parentTime - CYCLES_PER_FRAME;
    linkPeer_->linkedFrameCarry_ = childTime - CYCLES_PER_FRAME;
    return {true, elapsed};
}

} // namespace LOTUSGBA
