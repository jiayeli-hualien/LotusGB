#include "gba_system.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

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
constexpr uint32_t RFU_NORMAL_BAUD[] = {9600, 38400, 57600, 115200};
constexpr uint32_t CPU_CLOCK = 16'777'216;
// Network-mode compatibility policy: render/frontend time remains free-running,
// while guest progress is loosely bounded only during active multiplayer SIO.
// This is not a model of a cable synchronizing console frame clocks; it preserves
// actual SIO words and completion behavior while limiting guest-visible drift.
constexpr uint64_t REMOTE_LINK_MAX_FRAME_LEAD = 2;
constexpr uint64_t REMOTE_LINK_HOT_WINDOW_FRAMES = 8;
constexpr size_t REMOTE_LINK_MAX_QUEUED_STARTS = 64;
constexpr size_t REMOTE_LINK_MAX_QUEUED_WORDS = 64;
constexpr uint32_t EMERALD_LINK_TIMER3_CYCLES = 197 * 64;
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
    rfuLogin_.reset();
    rfuCommandSession_.reset();
    rfuPendingTxData_.reset();
    rfuPendingTxClockChange_ = false;
    rfuGuestSyncRequired_ = true;
    rfuGroupedRounds_.clear();
    rfuCommandGroup_.reset();
    rfuPendingTxBuffered_ = false;
    rfuParentAttemptPending_ = false;
    rfuParentReplyReady_ = false;
    rfuDataAckPayloadPending_ = false;
    rfuRadioAwaitingReply_ = false;
    rfuRadioReplyClockChange_ = false;
    rfuPendingConsumedSequence_.reset();
    rfuLoginActive_ = false;
    rfuCommandSessionActive_ = false;
    rfuMasterSwitchPending_ = false;
    rfuAdapterClockMaster_ = false;
    rfuAdapterAwaitingAck_ = false;
    rfuAdapterEventCommand_ = 0;
    rfuDisconnectEventPending_ = false;
    rfuDisconnectPayloadPending_ = false;
    rfuDisconnectEventSlotMask_ = 0;
    rfuDisconnectEventReason_ = 0;
    rfuBroadcastData_.fill(0);
    rfuSystemConfig_ = 0;
    rfuSearchingChildren_ = false;
    rfuSearchingParents_ = false;
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
        if (address == REG_SIOCNT && rfuCommandSessionActive_ &&
            hasEmeraldRfuProfile()) {
            const auto stored = io_[address - 0x04000000];
            return static_cast<uint8_t>((stored & ~0x04u) |
                (rfuSiLineHigh_ ? 0x04u : 0));
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
    if (memoryWriteObserver_)
        memoryWriteObserver_(address, value, 1);
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
        if ((address & ~1u) == REG_SIOMLT_SEND && remoteLinkChild_ &&
            remoteBufferedProtocolActive_)
            remoteChildAwaitingSend_ = false;
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
    if (memoryWriteObserver_)
        memoryWriteObserver_(address, value, 2);
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
    if (memoryWriteObserver_)
        memoryWriteObserver_(address, value, 4);
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
    MultiplayerWordPredictor predictWord,
    MultiplayerDataBlockSender sendDataBlock) {
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
    sendRemoteDataBlock_ = std::move(sendDataBlock);
    remoteBufferedProtocolRequested_ = false;
    remotePredictedWords_.clear();
    remotePendingWords_.clear();
    remotePendingDataBlocks_.clear();
    remotePendingLocalDataBlock_.reset();
    remoteActiveDataBlock_ = {};
    remoteOutgoingDataWords_ = {};
    remoteNextDataBlockSequence_ = 1;
    remoteExpectedDataBlockSequence_ = 1;
    remotePreviousDataChecksum_ = 0;
    remoteBufferedChecksumValid_ = false;
    remoteNextChildPulseCycle_ = 0;
    remoteStreamWordIndex_ = 0;
    remoteChildPulseIndex_ = 0;
    remoteBufferedBaudIndex_ = 3;
    remoteBufferedProtocolActive_ = false;
    remoteBufferedSwitchAfterTransfer_ = false;
    remoteBufferedHandshakeStartsPending_ = 0;
    remoteActiveDataBlockValid_ = false;
    remoteBufferedWordPending_ = false;
    remoteChildAwaitingSend_ = false;
    remotePeerWordReady_ = false;
    remoteReplySent_ = false;
    remotePendingStarts_.clear();
    remoteTransferSequence_ = 0;
    remoteRawTransferSequence_ = 0;
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

void GbaSystem::setRemoteMultiplayerBufferedProtocol(bool enabled) {
    if (!remoteLinkEnabled_)
        throw std::logic_error("GBA has no remote multiplayer link");
    remoteBufferedProtocolRequested_ = enabled;
    if (!enabled) {
        remoteBufferedProtocolActive_ = false;
        remoteBufferedSwitchAfterTransfer_ = false;
        remoteBufferedHandshakeStartsPending_ = 0;
        remotePendingDataBlocks_.clear();
        remoteActiveDataBlockValid_ = false;
    }
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
    // Once complete commands are buffered, both guests run independently.
    // During raw discovery, only pace the child: the master already waits for
    // real serial replies and must remain free to initiate the next transfer.
    // Pacing both can deadlock across guest serial resets with different
    // progress baselines. This intentionally changes child guest timing.
    if (remoteBufferedProtocolRequested_ &&
        (remoteBufferedProtocolActive_ || !remoteLinkChild_))
        return false;
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
        (!inMultiplayerMode() && !remoteBufferedProtocolRequested_) ||
        sequence != remoteRawTransferSequence_ + 1 || !sendRemoteWord_ ||
        baudIndex >= 4 ||
        remotePendingStarts_.size() >= REMOTE_LINK_MAX_QUEUED_STARTS) {
        multiplayerError_ = true;
        return false;
    }
    remoteRawTransferSequence_ = sequence;
    ++remoteTransferSequence_;
    markRemoteLinkActivity();
    if (remoteBufferedProtocolRequested_) {
        // Emerald's discovery polls can arrive while the other guest is
        // probing a different device. Complete them as an open cable slot
        // so the initiating guest can continue its discovery loop.
        if (!inMultiplayerMode() || remoteBufferedProtocolActive_) {
            sendRemoteWord_(sequence, 0xFFFF);
        } else {
            // The two games can enter discovery independently. The sender's
            // time since entry does not define a date on the receiver's clock.
            if (parentWord == 0x8FFF &&
                remoteBufferedHandshakeStartsPending_ !=
                    std::numeric_limits<uint32_t>::max())
                ++remoteBufferedHandshakeStartsPending_;
            remotePendingStarts_.push_back({sequence, parentWord, baudIndex,
                                           emulatedCycles_});
        }
        return true;
    }
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
    if (remoteBufferedProtocolActive_ && remoteLinkChild_) {
        beginRemoteBufferedChildTransfer();
        return;
    }
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
    remoteBufferedSwitchAfterTransfer_ = remoteBufferedProtocolRequested_ &&
        remoteLinkChild_ && start.parentWord == 0x8FFF &&
        multiplayerWords_[1] == 0xB9A0;
    if (remoteLinkChild_ && multiplayerCycles_ == 0) {
        sendRemoteWord_(remoteActiveSequence_, multiplayerWords_[1]);
        remoteReplySent_ = true;
        completeMultiplayerTransfer();
        if (remoteBufferedSwitchAfterTransfer_)
            activateRemoteBufferedProtocol();
    }
}

void GbaSystem::beginRemoteBufferedChildTransfer() {
    if (multiplayerBusy_ || remoteChildAwaitingSend_ || !remoteLinkConnected_ ||
        !inMultiplayerMode() || emulatedCycles_ < remoteNextChildPulseCycle_)
        return;
    ++remoteTransferSequence_;
    multiplayerWords_ = {prepareRemoteBufferedTransferWord(),
                         read16Impl(REG_SIOMLT_SEND)};
    const uint32_t baud = MULTIPLAYER_BAUD[remoteBufferedBaudIndex_];
    multiplayerCycles_ = (36 * CPU_CLOCK + baud - 1) / baud;
    multiplayerBusy_ = true;
    multiplayerError_ = false;
    remotePeerWordReady_ = true;
    remoteBufferedWordPending_ = true;
    for (uint32_t slot = 0; slot < 4; ++slot)
        storeIo16(REG_SIOMULTI0 + 2 * slot, 0xFFFF);

    if (++remoteChildPulseIndex_ < 9) {
        // Emerald arms Timer 3 after completing the serial callback. Include
        // local wire time and never catch up missed pulses in a burst.
        remoteNextChildPulseCycle_ = emulatedCycles_ + multiplayerCycles_ +
            EMERALD_LINK_TIMER3_CYCLES;
    } else {
        remoteChildPulseIndex_ = 0;
        const uint32_t vblankOffset = SCREEN_HEIGHT * CYCLES_PER_SCANLINE;
        uint32_t untilVBlank = (vblankOffset + CYCLES_PER_FRAME - frameCycle_) %
            CYCLES_PER_FRAME;
        if (!untilVBlank) untilVBlank = CYCLES_PER_FRAME;
        remoteNextChildPulseCycle_ = emulatedCycles_ + untilVBlank;
    }
}

bool GbaSystem::receiveRemoteMultiplayerWord(uint32_t sequence,
                                             uint16_t childWord) {
    const auto reject = [&](const char *reason, int predictedWord = -1) {
        char detail[320];
        std::snprintf(detail, sizeof(detail),
                      "reason=%s seq=%u word=%04X predicted=%d active=%u enabled=%d child=%d connected=%d buffered=%d busy=%d peer_ready=%d pending=%zu predictions=%zu",
                      reason, sequence, static_cast<unsigned>(childWord),
                      predictedWord, remoteActiveSequence_, remoteLinkEnabled_,
                      remoteLinkChild_, remoteLinkConnected_,
                      remoteBufferedProtocolActive_, multiplayerBusy_,
                      remotePeerWordReady_, remotePendingWords_.size(),
                      remotePredictedWords_.size());
        remoteWordRejection_ = detail;
        multiplayerError_ = true;
        return false;
    };
    const auto predicted = remotePredictedWords_.find(sequence);
    if (predicted != remotePredictedWords_.end()) {
        const uint16_t predictedWord = predicted->second;
        const bool matched = predictedWord == childWord;
        remotePredictedWords_.erase(predicted);
        if (!matched) {
            return reject("prediction_mismatch", predictedWord);
        }
        markRemoteLinkActivity();
        return true;
    }
    if (!remoteLinkEnabled_ || remoteLinkChild_ || !remoteLinkConnected_ ||
        remoteBufferedProtocolActive_) {
        return reject("link_state");
    }
    if (sequence > remoteActiveSequence_ ||
        (sequence == remoteActiveSequence_ && !multiplayerBusy_)) {
        if (sequence <= remoteActiveSequence_ ||
            sequence - remoteActiveSequence_ > REMOTE_LINK_MAX_QUEUED_WORDS) {
            return reject("sequence_window");
        }
        const auto pending = std::find_if(
            remotePendingWords_.begin(), remotePendingWords_.end(),
            [sequence](const RemoteMultiplayerWord &word) {
                return word.sequence == sequence;
            });
        if (pending != remotePendingWords_.end()) {
            if (pending->word != childWord) {
                return reject("duplicate_mismatch");
            }
            markRemoteLinkActivity();
            return true;
        }
        if (remotePendingWords_.size() >= REMOTE_LINK_MAX_QUEUED_WORDS) {
            return reject("pending_full");
        }
        remotePendingWords_.push_back({sequence, childWord});
        markRemoteLinkActivity();
        return true;
    }
    if (!multiplayerBusy_ || sequence != remoteActiveSequence_ ||
        remotePeerWordReady_) {
        return reject("unexpected_sequence_or_state");
    }
    markRemoteLinkActivity();
    multiplayerWords_[1] = childWord;
    remotePeerWordReady_ = true;
    remoteBufferedSwitchAfterTransfer_ =
        remoteBufferedProtocolRequested_ && !remoteLinkChild_ &&
        multiplayerWords_[0] == 0x8FFF && childWord == 0xB9A0;
    if (multiplayerCycles_ == 0) {
        completeMultiplayerTransfer();
        if (remoteBufferedSwitchAfterTransfer_)
            activateRemoteBufferedProtocol();
    }
    return true;
}

bool GbaSystem::receiveRemoteMultiplayerDataBlock(
    uint32_t sequence, const MultiplayerDataWords &words,
    uint16_t checksum) {
    return receiveRemoteMultiplayerDataBlockDetailed(sequence, words,
                                                      checksum).accepted();
}

GbaSystem::RemoteMultiplayerDataBlockResult
GbaSystem::receiveRemoteMultiplayerDataBlockDetailed(
    uint32_t sequence, const MultiplayerDataWords &words,
    uint16_t checksum) {
    RemoteMultiplayerDataBlockResult result;
    result.expectedSequence = remoteExpectedDataBlockSequence_;
    result.queuedBlocks = remotePendingDataBlocks_.size();
    result.activeBlock = remoteActiveDataBlockValid_;
    uint16_t calculatedChecksum = 0;
    for (uint16_t word : words)
        calculatedChecksum = static_cast<uint16_t>(calculatedChecksum + word);
    result.calculatedChecksum = calculatedChecksum;

    const auto reject = [this, &result](
                            RemoteMultiplayerDataBlockRejectReason reason) {
        if (reason != RemoteMultiplayerDataBlockRejectReason::QueueFull)
            multiplayerError_ = true;
        result.reason = reason;
        return result;
    };
    if (!remoteLinkEnabled_)
        return reject(RemoteMultiplayerDataBlockRejectReason::LinkNotConfigured);
    if (!remoteLinkConnected_)
        return reject(RemoteMultiplayerDataBlockRejectReason::LinkDisconnected);
    if (!remoteBufferedProtocolRequested_)
        return reject(RemoteMultiplayerDataBlockRejectReason::
                          BufferedProtocolNotRequested);
    if (sequence != remoteExpectedDataBlockSequence_)
        return reject(RemoteMultiplayerDataBlockRejectReason::SequenceMismatch);
    if (remotePendingDataBlocks_.size() >=
        REMOTE_MULTIPLAYER_DATA_BLOCK_QUEUE_LIMIT)
        return reject(RemoteMultiplayerDataBlockRejectReason::QueueFull);
    if (calculatedChecksum != checksum)
        return reject(RemoteMultiplayerDataBlockRejectReason::ChecksumMismatch);

    result.reason = RemoteMultiplayerDataBlockRejectReason::None;
    if (!remoteBufferedProtocolActive_) {
        // Reliable ordered packets retain session-wide sequence numbers across
        // guest serial resets. Consume late old-attempt packets without
        // delivering them to the reset parser. The child may hold blocks that
        // follow a queued 0x8FFF handoff until its local serial callback
        // confirms the same 0x8FFF/0xB9A0 transition.
        ++remoteExpectedDataBlockSequence_;
        if (remoteLinkChild_ && remoteBufferedHandshakeStartsPending_ > 0) {
            remotePendingDataBlocks_.push_back({sequence, words, checksum});
            markRemoteLinkActivity();
        }
        return result;
    }
    remotePendingDataBlocks_.push_back({sequence, words, checksum});
    ++remoteExpectedDataBlockSequence_;
    markRemoteLinkActivity();
    return result;
}

void GbaSystem::configureRemoteRfuLink(
    uint16_t localId, uint16_t peerId,
    RfuAdvertisementSender sendAdvertisement,
    RfuConnectionSender sendConnection, RfuDataSender sendData,
    RfuDataAcceptedSender sendDataAccepted,
    RfuDisconnectSender sendDisconnect, RfuCompletionMode completionMode) {
    if (!hasEmeraldRfuProfile() || remoteRfuEnabled_ ||
        remoteLinkEnabled_ || linkPeer_ || localId == 0 || peerId == 0 ||
        localId == peerId || !sendAdvertisement || !sendConnection ||
        !sendData)
        throw std::invalid_argument("Invalid Emerald remote RFU link");
    rfuCompletionMode_ = completionMode;
    remoteRfuEnabled_ = true;
    rfuLocalId_ = localId;
    rfuPeerId_ = peerId;
    rfuGuestFrameCount_ = 0;
    rfuNextAdvertisementSequence_ = 1;
    rfuExpectedAdvertisementSequence_ = 1;
    rfuNextConnectionSequence_ = 1;
    rfuExpectedIncomingConnectionSequence_ = 1;
    rfuPendingConnectionSequence_ = 0;
    rfuNextDataSequence_ = 1;
    rfuPeerAdvertisementKnown_ = false;
    rfuConnectionPending_ = false;
    rfuConnected_ = false;
    rfuAdapterEventCommand_ = 0;
    rfuDisconnectEventPending_ = false;
    rfuDisconnectPayloadPending_ = false;
    rfuDisconnectEventSlotMask_ = 0;
    rfuDisconnectEventReason_ = 0;
    rfuAdapterClockMaster_ = false;
    rfuAdapterAwaitingAck_ = false;
    rfuDataChannel_.reset(true);
    rfuRetainedTxData_.reset();
    rfuPendingTxData_.reset();
    rfuPendingTxClockChange_ = false;
    rfuGuestSyncRequired_ = true;
    rfuGroupedRounds_.clear();
    rfuCommandGroup_.reset();
    rfuPendingTxBuffered_ = false;
    rfuParentAttemptPending_ = false;
    rfuParentReplyReady_ = false;
    rfuDataAckPayloadPending_ = false;
    rfuRadioAwaitingReply_ = false;
    rfuRadioReplyClockChange_ = false;
    rfuPendingRadioEvents_ = 0;
    sendRfuAdvertisement_ = std::move(sendAdvertisement);
    sendRfuConnection_ = std::move(sendConnection);
    sendRfuData_ = std::move(sendData);
    sendRfuDataAccepted_ = std::move(sendDataAccepted);
    sendRfuDisconnect_ = std::move(sendDisconnect);
}

void GbaSystem::setRfuDiagnosticLogger(
    std::function<void(const std::string &)> logger, bool detailed) {
    rfuDetailedDiagnostics_ = detailed;
    rfuDiagnosticLogger_ = std::move(logger);
    rfuTransferCount_ = 0;
    rfuDiagnosticCount_ = 0;
    rfuRecentDiagnostics_.clear();
}

void GbaSystem::setRemoteRfuEventPump(std::function<void()> pump) {
    if (!remoteRfuEnabled_)
        throw std::logic_error("GBA has no remote RFU link");
    rfuNetworkPump_ = std::move(pump);
}

void GbaSystem::logRfuDiagnostic(const std::string &message, bool essential) {
    constexpr uint32_t MAX_RFU_DIAGNOSTICS = 4096;
    if (!rfuDiagnosticLogger_)
        return;
    // Connection and clock evidence must survive long discovery sessions.
    if (essential) {
        rfuDiagnosticLogger_(message);
        return;
    }
    if (rfuDiagnosticCount_ >= MAX_RFU_DIAGNOSTICS) {
        // Retain bounded context after the startup trace limit. Emerald can
        // reset much later, and losing these words hides the failing exchange.
        constexpr size_t RECENT_WORDS = 32;
        if (rfuRecentDiagnostics_.size() == RECENT_WORDS)
            rfuRecentDiagnostics_.pop_front();
        rfuRecentDiagnostics_.push_back(message);
        return;
    }
    ++rfuDiagnosticCount_;
    rfuDiagnosticLogger_(message);
    if (rfuDiagnosticCount_ == MAX_RFU_DIAGNOSTICS)
        rfuDiagnosticLogger_("RFU trace limit reached; further SIO words suppressed");
}

// Opt-in trace records the bytes at each ownership boundary, not decoded
// game state. It neither changes packet admission nor reads guest memory via
// the timed CPU bus. Commercial-game traces must stay outside the repository.
void GbaSystem::logRfuDataDiagnostic(
    const char *boundary, uint32_t sequence, uint8_t payloadBytes,
    const std::vector<uint32_t> &words, bool senderParent) {
    if (!rfuDetailedDiagnostics_ || !rfuDiagnosticLogger_)
        return;
    constexpr char HEX[] = "0123456789ABCDEF";
    std::string hex;
    for (size_t index = 0; index < payloadBytes && index / 4 < words.size(); ++index) {
        const uint8_t byte = static_cast<uint8_t>(words[index / 4] >> ((index % 4) * 8));
        hex += HEX[byte >> 4];
        hex += HEX[byte & 15];
    }
    char trace[384];
    std::snprintf(trace, sizeof(trace),
        "RFU payload: frame=%llu cycles=%llu boundary=%s seq=%u sender=%s "
        "bytes=%u pc=%08X lr=%08X queued=%zu awaiting_reply=%u reply_ready=%u hex=",
        static_cast<unsigned long long>(rfuGuestFrameCount_),
        static_cast<unsigned long long>(emulatedCycles_), boundary, sequence,
        senderParent ? "parent" : "child", unsigned(payloadBytes),
        cpu_.programCounter(), cpu_.registerValue(14), rfuDataChannel_.queuedMessages(),
        unsigned(rfuRadioAwaitingReply_), unsigned(rfuParentReplyReady_));
    rfuDiagnosticLogger_(std::string(trace) + (hex.empty() ? "-" : hex));
}

bool GbaSystem::rfuParentCanDispatch() const {
    if (rfuCompletionMode_ != RfuCompletionMode::GroupedCommands)
        return !rfuRadioAwaitingReply_;
    return rfuGroupedRounds_.size() < GbaRfuDataChannel::MAX_QUEUED_MESSAGES &&
        rfuGroupedRounds_.size() + rfuDataChannel_.queuedMessages() <
            GbaRfuDataChannel::MAX_QUEUED_MESSAGES &&
        (rfuGroupedRounds_.empty() || rfuGroupedRounds_.back().buffered);
}

bool GbaSystem::dispatchRfuAsync(uint32_t sequence, uint8_t payloadBytes,
    const std::vector<uint32_t> &words, bool clockChange) {
    if (!sendRfuData_(sequence, payloadBytes, words)) return false;
    // Intentional adapter compatibility policy: immutable local transport
    // ownership completes TX; independent peer packets publish RX later.
    // No NI ACK, application block, or readiness payload is synthesized.
    // Unlike grouped prefixes, this policy has no per-round reply pairing.
    // See docs/gba-rfu-protocol.md: Asynchronous adapter experiment.
    if (rfuIsParent_ && clockChange) rfuParentReplyReady_ = true;
    logRfuDataDiagnostic("tx_dispatch", sequence, payloadBytes, words, rfuIsParent_);
    return true;
}

bool GbaSystem::dispatchRfuParent(uint32_t sequence, uint8_t payloadBytes,
    const std::vector<uint32_t> &words, bool clockChange, bool buffered) {
    if (!rfuParentCanDispatch()) return false;
    rfuGroupedRounds_.push_back({sequence, clockChange, buffered});
    rfuRadioAwaitingReply_ = true;
    if (!sendRfuData_(sequence, payloadBytes, words)) {
        rfuGroupedRounds_.pop_back();
        rfuRadioAwaitingReply_ = !rfuGroupedRounds_.empty();
        return false;
    }
    // Intentional compatibility behavior: a prefix's radio slot ACK means
    // immutable local/transport storage, not remote receipt. The last block
    // fragment and readiness/unknown commands fence all preceding replies.
    // Guest NI/application ACK contents are never generated here.
    // See docs/gba-rfu-protocol.md: Grouped command pipeline.
    if (clockChange && buffered)
        rfuParentReplyReady_ = true;
    if (rfuDetailedDiagnostics_) {
        char trace[160];
        std::snprintf(trace, sizeof(trace), "RFU group dispatch: seq=%u completion=%s pending=%zu",
                      sequence, buffered ? "buffered-prefix" : "peer-fence", rfuGroupedRounds_.size());
        logRfuDiagnostic(trace, true);
    }
    logRfuDataDiagnostic("tx_dispatch", sequence, payloadBytes, words, true);
    return true;
}

bool GbaSystem::rfuParentPollReady() const {
    if (rfuCompletionMode_ == RfuCompletionMode::GroupedCommands ||
        rfuCompletionMode_ == RfuCompletionMode::AsyncAdapter)
        return rfuIsParent_ && rfuParentAttemptPending_ && rfuParentReplyReady_;
    return rfuIsParent_ && rfuCompletionMode_ != RfuCompletionMode::RemoteReply &&
        rfuParentAttemptPending_ &&
        (rfuCompletionMode_ != RfuCompletionMode::GuardedLocalPoll ||
         !rfuGuestSyncRequired_ || rfuParentReplyReady_);
}

void GbaSystem::observeRfuSyncPayload(
    bool senderParent, uint8_t payloadBytes, const std::vector<uint32_t> &words) {
    if (rfuCompletionMode_ != RfuCompletionMode::GuardedLocalPoll || !payloadBytes)
        return;
    const auto byte = [&words](size_t index) {
        return static_cast<uint8_t>(words[index / 4] >> ((index % 4) * 8));
    };
    const size_t headerBytes = senderParent ? 3u : 2u;
    // A one-byte ghost send is no evidence of an application phase change.
    if (senderParent && payloadBytes == 1)
        return;
    bool critical = false, ownKeys = false;
    size_t offset = 0;
    while (offset + headerBytes <= payloadBytes && words.size() * 4 >= payloadBytes) {
        uint32_t header = 0;
        for (size_t i = 0; i < headerBytes; ++i)
            header |= uint32_t{byte(offset + i)} << (i * 8);
        const size_t length = header & (senderParent ? 127u : 31u);
        const unsigned state = (header >> (senderParent ? 14 : 10)) & 15;
        offset += headerBytes;
        if (offset + length > payloadBytes) { critical = true; break; }
        if (state != 4 || length != (senderParent ? 70u : 14u)) {
            critical = true; // NI and unrecognized data retain synchronous completion.
        } else {
            for (size_t row = 0; row < length / 14; ++row) {
                const size_t at = offset + row * 14;
                uint16_t command = static_cast<uint16_t>(byte(at) | (uint16_t{byte(at + 1)} << 8));
                if (!senderParent) command &= 0xFF1F; // Child application sequence bits.
                if (command && command != 0xBE00) critical = true;
                if (senderParent && row == 0 && command == 0xBE00) ownKeys = true;
            }
        }
        offset += length;
    }
    if (offset != payloadBytes) critical = true;
    // Emerald's trade UI conditionally installs a standby callback only after
    // its block sender is idle, then advances the UI unconditionally. Letting
    // local fade frames outrun that callback loses the standby handoff.
    // Keep the entire data phase synchronous, including idle gaps. Only an
    // explicit parent-owned pure key stream opens local polling again.
    // This profile-specific compatibility boundary is documented in
    // docs/gba-rfu-protocol.md; it is not a hardware RFU timing claim.
    const bool required = critical ? true : (ownKeys ? false : rfuGuestSyncRequired_);
    if (required != rfuGuestSyncRequired_) {
        rfuGuestSyncRequired_ = required;
        logRfuDiagnostic(required ? "RFU sync boundary: data phase requires peer completion"
                                  : "RFU sync boundary: pure key phase permits local polls", true);
    }
}

void GbaSystem::logRfuFrameDiagnostic() {
    if (!rfuDetailedDiagnostics_ || !rfuDiagnosticLogger_ || rfuGuestFrameCount_ % 60)
        return;
    size_t black = 0;
    uint32_t hash = 2166136261u;
    for (uint16_t pixel : frameBuffer_) {
        if ((pixel & 0x7FFF) == 0) ++black;
        hash = (hash ^ pixel) * 16777619u;
    }
    char trace[512];
    std::snprintf(trace, sizeof(trace),
        "RFU frame: frame=%llu cycles=%llu role=%s pc=%08X lr=%08X sp=%08X "
        "dispcnt=%04X bldcnt=%04X bldalpha=%04X bldy=%04X black=%zu "
        "pixels=%zu fb_hash=%08X siocnt=%04X ie=%04X if=%04X ime=%04X "
        "queued=%zu next_rx=%u next_tx=%u awaiting_reply=%u reply_ready=%u sync_required=%u",
        static_cast<unsigned long long>(rfuGuestFrameCount_),
        static_cast<unsigned long long>(emulatedCycles_), rfuIsParent_ ? "parent" : "child",
        cpu_.programCounter(), cpu_.registerValue(14), cpu_.registerValue(13),
        loadIo16(REG_DISPCNT), loadIo16(0x04000050), loadIo16(0x04000052),
        loadIo16(0x04000054), black, frameBuffer_.size(), hash, loadIo16(REG_SIOCNT),
        loadIo16(REG_IE), loadIo16(REG_IF), loadIo16(REG_IME),
        rfuDataChannel_.queuedMessages(), rfuDataChannel_.expectedSequence(),
        rfuNextDataSequence_, unsigned(rfuRadioAwaitingReply_), unsigned(rfuParentReplyReady_),
        unsigned(rfuGuestSyncRequired_));
    rfuDiagnosticLogger_(trace);
}

void GbaSystem::logRfuDisconnectDiagnostic(uint8_t command) {
    if (!rfuDiagnosticLogger_)
        return;
    rfuDiagnosticLogger_("RFU disconnect context begin (before state clear)");
    for (const auto &message : rfuRecentDiagnostics_)
        rfuDiagnosticLogger_(message);
    rfuRecentDiagnostics_.clear();
    char trace[320];
    std::snprintf(trace, sizeof(trace),
                  "RFU disconnect: command=%02X pc=%08X lr=%08X connected=%u "
                  "pending=%u queued=%zu role=%s slot=%u next_rx=%u "
                  "next_tx=%u pending_events=%u clock=%s awaiting_ack=%u "
                  "serial_busy=%u siocnt=%04X rcnt=%04X",
                  command, cpu_.registerValue(15), cpu_.registerValue(14),
                  unsigned(rfuConnected_), unsigned(rfuConnectionPending_),
                  rfuDataChannel_.queuedMessages(),
                  rfuIsParent_ ? "parent" : "child",
                  unsigned(rfuConnectedSlot_),
                  rfuDataChannel_.expectedSequence(),
                  rfuNextDataSequence_, rfuPendingRadioEvents_,
                  rfuAdapterClockMaster_ ? "adapter" : "guest",
                  unsigned(rfuAdapterAwaitingAck_),
                  unsigned(normalSerialBusy_), read16Impl(REG_SIOCNT),
                  read16Impl(REG_RCNT));
    rfuDiagnosticLogger_(trace);
}

bool GbaSystem::receiveRemoteRfuAdvertisement(
    uint32_t sequence, uint16_t id, const RfuAdvertisement &data) {
    if (!remoteRfuEnabled_ || sequence != rfuExpectedAdvertisementSequence_ ||
        id != rfuPeerId_) {
        return false;
    }
    rfuPeerAdvertisement_ = data;
    rfuPeerAdvertisementKnown_ = true;
    ++rfuExpectedAdvertisementSequence_;
    return true;
}

bool GbaSystem::receiveRemoteRfuConnectionRequest(
    uint32_t sequence, uint16_t sourceId, uint16_t targetId) {
    const char *reason = !remoteRfuEnabled_ ? "disabled" :
        sequence != rfuExpectedIncomingConnectionSequence_ ? "sequence" :
        sourceId != rfuPeerId_ ? "source_id" :
        targetId != rfuLocalId_ ? "target_id" :
        !rfuSearchingChildren_ ? "not_listening" :
        rfuConnected_ ? "already_connected" : "accepted";
    char trace[384];
    std::snprintf(trace, sizeof(trace),
        "RFU connection request: seq=%u expected=%u source=%u target=%u "
        "local=%u peer=%u searching_children=%u searching_parents=%u "
        "connected=%u pending=%u reason=%s",
        sequence, rfuExpectedIncomingConnectionSequence_, sourceId, targetId,
        rfuLocalId_, rfuPeerId_, unsigned(rfuSearchingChildren_),
        unsigned(rfuSearchingParents_), unsigned(rfuConnected_),
        unsigned(rfuConnectionPending_), reason);
    logRfuDiagnostic(trace, true);
    if (!remoteRfuEnabled_ ||
        sequence != rfuExpectedIncomingConnectionSequence_ ||
        sourceId != rfuPeerId_ || targetId != rfuLocalId_)
        return false;
    ++rfuExpectedIncomingConnectionSequence_;
    if (!rfuSearchingChildren_ || rfuConnected_)
        return false;
    rfuConnected_ = true;
    rfuIsParent_ = true;
    rfuDataChannel_.setConnection(true, true);
    rfuConnectedId_ = sourceId;
    rfuConnectedSlot_ = 0;
    return true;
}

bool GbaSystem::receiveRemoteRfuConnectionResult(
    uint32_t sequence, uint16_t targetId, uint8_t slot, bool accepted) {
    char trace[256];
    std::snprintf(trace, sizeof(trace),
        "RFU connection result: seq=%u expected=%u target=%u local=%u "
        "slot=%u accepted=%u enabled=%u pending=%u connected=%u",
        sequence, rfuPendingConnectionSequence_, targetId, rfuLocalId_,
        unsigned(slot), unsigned(accepted), unsigned(remoteRfuEnabled_),
        unsigned(rfuConnectionPending_), unsigned(rfuConnected_));
    logRfuDiagnostic(trace, true);
    if (!remoteRfuEnabled_ || !rfuConnectionPending_ ||
        sequence != rfuPendingConnectionSequence_ || targetId != rfuLocalId_ ||
        slot > 3) {
        return false;
    }
    rfuConnectionPending_ = false;
    if (accepted) {
        rfuConnected_ = true;
        rfuIsParent_ = false;
        rfuDataChannel_.setConnection(true, false);
        rfuConnectedId_ = rfuPeerId_;
        rfuConnectedSlot_ = slot;
    }
    return true;
}

GbaSystem::RfuDataReceiveResult GbaSystem::receiveRemoteRfuData(
    uint32_t sequence, uint8_t payloadBytes,
    const std::vector<uint32_t> &frameWords) {
    // Reference children retain an immutable reply; async children retain an
    // immutable pending uplink. Neither can admit new RX over unfinished TX
    // or lose an event by admitting another packet during its A8/0x26 cycle.
    if (remoteRfuEnabled_ && rfuConnected_ && !rfuIsParent_ &&
        (rfuPendingTxData_ ||
         ((rfuCompletionMode_ == RfuCompletionMode::GroupedCommands ||
           rfuCompletionMode_ == RfuCompletionMode::AsyncAdapter) &&
          (rfuPendingRadioEvents_ || rfuAdapterAwaitingAck_ ||
           rfuPendingConsumedSequence_ || rfuDataChannel_.hasReceived()))))
        return RfuDataReceiveResult::QueueFull;
    if (remoteRfuEnabled_ && rfuConnected_ && rfuIsParent_ &&
        rfuCompletionMode_ == RfuCompletionMode::GroupedCommands &&
        (rfuGroupedRounds_.empty() || rfuGroupedRounds_.front().sequence != sequence))
        return RfuDataReceiveResult::UnexpectedSequence;
    const auto result = rfuDataChannel_.receive(sequence, payloadBytes, frameWords);
    if (result != RfuDataReceiveResult::Accepted)
        return result;
    logRfuDataDiagnostic("rx_admit", sequence, payloadBytes, frameWords, !rfuIsParent_);
    if (rfuCompletionMode_ == RfuCompletionMode::AsyncAdapter) {
        // Independent ordered streams: a client write is not the reply to a
        // particular parent packet. Local TX completion never creates RX.
        if (rfuIsParent_) rfuDataChannel_.advanceParentRadioCycle();
        else ++rfuPendingRadioEvents_;
        if (sendRfuDataAccepted_) sendRfuDataAccepted_(sequence);
        return result;
    }
    if (rfuIsParent_) {
        observeRfuSyncPayload(false, payloadBytes, frameWords);
        if (rfuCompletionMode_ == RfuCompletionMode::GroupedCommands) {
            const auto round = rfuGroupedRounds_.front();
            rfuGroupedRounds_.pop_front();
            rfuDataChannel_.advanceParentRadioCycle();
            rfuRadioAwaitingReply_ = !rfuGroupedRounds_.empty();
            if (!round.buffered) {
                if (round.clockChange) {
                    rfuParentReplyReady_ = true;
                    rfuCommandGroup_.completeFence();
                }
                // An ordinary 0x24 never entered the command-group planner.
                // Its reply must preserve a newer queued 0x25 initializer.
            }
        } else if (rfuRadioAwaitingReply_) {
            // The reference model completes both directions at this real
            // child-reply boundary, rather than pipelining buffered prefixes.
            rfuDataChannel_.advanceParentRadioCycle();
            if (rfuRadioReplyClockChange_) {
                if (rfuCompletionMode_ != RfuCompletionMode::RemoteReply)
                    rfuParentReplyReady_ = true;
                else
                    ++rfuPendingRadioEvents_;
            }
            rfuRadioAwaitingReply_ = false;
            rfuRadioReplyClockChange_ = false;
        }
    } else {
        // Snapshot the scheduled child TX when a parent exchange arrives.
        // Subsequent guest writes can prepare the next exchange, but cannot
        // mutate this reply. An empty child slot still completes the exchange.
        const uint8_t count = rfuRetainedTxData_ ? rfuRetainedTxData_->payloadBytes : 0;
        const std::vector<uint32_t> reply = rfuRetainedTxData_
            ? rfuRetainedTxData_->frameWords : std::vector<uint32_t>{};
        // One scheduled child uplink belongs to one selected exchange. NI
        // retries are fresh guest writes; do not invent new application data
        // by retransmitting an already selected child slot on every cycle.
        rfuRetainedTxData_.reset();
        if (sendRfuData_(rfuNextDataSequence_, count, reply)) {
            logRfuDataDiagnostic("tx_dispatch", rfuNextDataSequence_, count, reply, false);
            ++rfuNextDataSequence_;
        } else {
            rfuPendingTxData_ = RemoteRfuData{rfuNextDataSequence_, count, reply};
            rfuPendingTxClockChange_ = false;
        }
        ++rfuPendingRadioEvents_;
    }
    // Admission credit is independent of guest reads and NI acknowledgements.
    if (sendRfuDataAccepted_)
        sendRfuDataAccepted_(sequence);
    return result;
}

void GbaSystem::receiveRemoteRfuDisconnect() {
    const bool canClockDisconnectEvent = remoteRfuEnabled_ && rfuConnected_ &&
        rfuAdapterClockMaster_ && !rfuAdapterAwaitingAck_;
    if (canClockDisconnectEvent) {
        rfuDisconnectEventPending_ = true;
        rfuDisconnectPayloadPending_ = false;
        rfuDisconnectEventSlotMask_ = static_cast<uint8_t>(
            1u << rfuConnectedSlot_);
        rfuDisconnectEventReason_ = 0; // REASON_DISCONNECTED.
        rfuPendingRadioEvents_ = 1;
        logRfuDiagnostic(
            "RFU peer disconnected; scheduling 0x29 adapter event");
    } else {
        rfuDisconnectEventPending_ = false;
        rfuDisconnectPayloadPending_ = false;
        rfuPendingRadioEvents_ = 0;
        if (!rfuAdapterAwaitingAck_)
            rfuAdapterClockMaster_ = false;
    }
    rfuConnected_ = false;
    rfuIsParent_ = false;
    rfuConnectionPending_ = false;
    rfuConnectedId_ = 0;
    rfuPeerAdvertisementKnown_ = false;
    rfuDataChannel_.disconnect();
    rfuRetainedTxData_.reset();
    rfuPendingTxData_.reset();
    rfuPendingTxClockChange_ = false;
    rfuGuestSyncRequired_ = true;
    rfuGroupedRounds_.clear();
    rfuCommandGroup_.reset();
    rfuPendingTxBuffered_ = false;
    rfuParentAttemptPending_ = false;
    rfuParentReplyReady_ = false;
    if (!rfuAdapterAwaitingAck_)
        rfuDataAckPayloadPending_ = false;
    rfuRadioAwaitingReply_ = false;
    rfuRadioReplyClockChange_ = false;
    rfuMasterSwitchPending_ = false;
}

void GbaSystem::disconnectRemoteRfuLink() {
    rfuNetworkPump_ = {};
    remoteRfuEnabled_ = false;
    rfuConnected_ = false;
    rfuConnectionPending_ = false;
    rfuPeerAdvertisementKnown_ = false;
    rfuDataChannel_.reset();
    rfuRetainedTxData_.reset();
    rfuPendingTxData_.reset();
    rfuPendingTxClockChange_ = false;
    rfuGuestSyncRequired_ = true;
    rfuGroupedRounds_.clear();
    rfuCommandGroup_.reset();
    rfuPendingTxBuffered_ = false;
    rfuParentAttemptPending_ = false;
    rfuParentReplyReady_ = false;
    rfuDataAckPayloadPending_ = false;
    rfuRadioAwaitingReply_ = false;
    rfuRadioReplyClockChange_ = false;
    rfuPendingRadioEvents_ = 0;
    rfuAdapterClockMaster_ = false;
    rfuAdapterAwaitingAck_ = false;
    rfuAdapterEventCommand_ = 0;
    rfuDisconnectEventPending_ = false;
    rfuDisconnectPayloadPending_ = false;
    rfuDisconnectEventSlotMask_ = 0;
    rfuDisconnectEventReason_ = 0;
    sendRfuAdvertisement_ = {};
    sendRfuConnection_ = {};
    sendRfuData_ = {};
    sendRfuDataAccepted_ = {};
    sendRfuDisconnect_ = {};
}

void GbaSystem::resetRemoteBufferedProtocol() {
    abortMultiplayerTransfer();
    // Each guest can reset its serial parser without leaving multiplayer mode.
    // The next raw exchange needs a fresh frame-progress baseline.
    remoteLinkHot_ = false;
    remoteBufferedProtocolActive_ = false;
    remoteBufferedSwitchAfterTransfer_ = false;
    remoteBufferedHandshakeStartsPending_ = 0;
    remoteBufferedWordPending_ = false;
    remoteChildAwaitingSend_ = false;
    remotePendingDataBlocks_.clear();
    remoteActiveDataBlock_ = {};
    remoteActiveDataBlockValid_ = false;
    remoteOutgoingDataWords_.fill(0);
    remoteStreamWordIndex_ = 0;
    remotePreviousDataChecksum_ = 0;
    remoteBufferedChecksumValid_ = false;
    remoteNextChildPulseCycle_ = 0;
    remoteChildPulseIndex_ = 0;
    multiplayerError_ = false;
}

void GbaSystem::activateRemoteBufferedProtocol() {
    if (!remoteBufferedProtocolRequested_ || remoteBufferedProtocolActive_)
        return;
    remoteBufferedProtocolActive_ = true;
    remoteBufferedSwitchAfterTransfer_ = false;
    remoteBufferedHandshakeStartsPending_ = 0;
    remoteBufferedWordPending_ = false;
    remoteStreamWordIndex_ = 0;
    remoteOutgoingDataWords_.fill(0);
    remotePreviousDataChecksum_ = 0;
    remoteBufferedChecksumValid_ = false;
    remoteActiveDataBlock_ = {};
    remoteActiveDataBlockValid_ = false;

    if (remoteLinkChild_) {
        // Wait for the handshake callback's SEND publication before starting
        // local cadence. Subsequently SerialCB calls DoRecv before DoSend:
        // that next SEND write marks the receive registers consumed, even if
        // IF was acknowledged much earlier in the interrupt dispatcher.
        remoteChildAwaitingSend_ = true;
        // Emerald's remote station receives nine serial interrupts per link
        // command group: one VBlank start followed by the master's Timer 3
        // retries. Recreate that local cadence without waiting for packets.
        // These empty groups contain zero command words and a zero checksum;
        // they do not consume or replace queued peer data.
        const uint32_t vblankOffset = SCREEN_HEIGHT * CYCLES_PER_SCANLINE;
        const uint32_t frameOffset = frameCycle_;
        // Begin on the next full frame boundary. Starting at the imminent
        // VBlank can make the tail of a frame look like a partial Emerald
        // command group and mix handshake words into its first data block.
        const uint32_t untilVBlank = CYCLES_PER_FRAME - frameOffset +
            vblankOffset;
        remoteNextChildPulseCycle_ = emulatedCycles_ +
            untilVBlank;
        remoteChildPulseIndex_ = 0;
    }
}

uint16_t GbaSystem::prepareRemoteBufferedTransferWord() {
    if (remoteStreamWordIndex_ == 0) {
        if (!remotePendingDataBlocks_.empty()) {
            remoteActiveDataBlock_ = remotePendingDataBlocks_.front();
            remotePendingDataBlocks_.pop_front();
            remoteActiveDataBlockValid_ = true;
        } else {
            // Emerald treats zero command slots as an empty receive group.
            // Keep the group's checksum phase aligned while retaining every
            // late peer block in the ordered queue for a later group.
            remoteActiveDataBlock_ = {};
            remoteActiveDataBlockValid_ = false;
        }
        return remotePreviousDataChecksum_;
    }
    return remoteActiveDataBlock_.words[remoteStreamWordIndex_ - 1];
}

void GbaSystem::finishRemoteBufferedTransferWord() {
    const uint8_t wordIndex = remoteStreamWordIndex_;
    const uint16_t local = multiplayerWords_[remoteLinkChild_ ? 1 : 0];
    if (wordIndex == 0) {
        // The guest checks the previous group's combined checksum first,
        // then receives eight words. Each local virtual cable includes its
        // own outgoing words and the actual queued peer words it delivered.
        // This guest checksum is distinct from a packet's own-payload sum.
        if (remoteBufferedChecksumValid_ && local != remotePreviousDataChecksum_)
            multiplayerError_ = true;
        remotePreviousDataChecksum_ = 0;
    } else {
        remoteOutgoingDataWords_[wordIndex - 1] = local;
        remotePreviousDataChecksum_ = static_cast<uint16_t>(remotePreviousDataChecksum_ +
            multiplayerWords_[0] + multiplayerWords_[1]);
    }
    if (wordIndex == 8) {
        const bool nonempty = std::any_of(remoteOutgoingDataWords_.begin(),
            remoteOutgoingDataWords_.end(), [](uint16_t w) { return w != 0; });
        uint16_t checksum = 0;
        for (uint16_t w : remoteOutgoingDataWords_) checksum = static_cast<uint16_t>(checksum + w);
        // Zero command slots carry no application message. Generate their
        // local wire cadence without filling the peer queue with idle frames.
        if (nonempty && sendRemoteDataBlock_) {
            remotePendingLocalDataBlock_ = RemoteMultiplayerDataBlock{
                remoteNextDataBlockSequence_, remoteOutgoingDataWords_, checksum};
            servicePendingRemoteDataBlock();
        }
        remoteBufferedChecksumValid_ = true;
        remoteActiveDataBlockValid_ = false;
        remoteStreamWordIndex_ = 0;
    } else {
        ++remoteStreamWordIndex_;
    }
}

bool GbaSystem::servicePendingRemoteDataBlock() {
    if (!remotePendingLocalDataBlock_)
        return true;
    if (!sendRemoteDataBlock_)
        return false;
    const auto &pending = *remotePendingLocalDataBlock_;
    if (!sendRemoteDataBlock_(pending.sequence, pending.words,
                              pending.checksum))
        return false;
    ++remoteNextDataBlockSequence_;
    remotePendingLocalDataBlock_.reset();
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
    remoteLinkConnected_ = false;
    abortMultiplayerTransfer();
    remoteLinkEnabled_ = false;
    remoteLinkChild_ = false;
    remoteBufferedProtocolRequested_ = false;
    remoteBufferedProtocolActive_ = false;
    remoteBufferedSwitchAfterTransfer_ = false;
    remoteBufferedHandshakeStartsPending_ = 0;
    remoteBufferedWordPending_ = false;
    remotePendingDataBlocks_.clear();
    remotePendingLocalDataBlock_.reset();
    remoteActiveDataBlockValid_ = false;
    remotePeerReady_ = false;
    remotePeerWordReady_ = false;
    sendRemoteStart_ = {};
    sendRemoteWord_ = {};
    sendRemoteReady_ = {};
    sendRemoteFrame_ = {};
    sendRemoteDataBlock_ = {};
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
    if (address == REG_RCNT && hasEmeraldRfuProfile() &&
        value == 0x80A2) {
        if (!rfuLoginActive_)
            logRfuDiagnostic("RFU login started by RCNT=80A2");
        rfuLogin_.reset();
        rfuCommandSession_.reset();
        rfuPendingConsumedSequence_.reset();
        rfuLoginActive_ = true;
        rfuCommandSessionActive_ = false;
        rfuMasterSwitchPending_ = false;
        rfuAdapterClockMaster_ = false;
        rfuAdapterAwaitingAck_ = false;
        rfuAdapterEventCommand_ = 0;
        rfuGuestSyncRequired_ = true;
        rfuGroupedRounds_.clear();
        rfuCommandGroup_.reset();
        rfuPendingTxBuffered_ = false;
        rfuParentAttemptPending_ = false;
        rfuParentReplyReady_ = false;
        rfuDataAckPayloadPending_ = false;
        rfuDisconnectEventPending_ = false;
        rfuDisconnectPayloadPending_ = false;
        rfuSiLineHigh_ = false;
    } else if (address == REG_SIOCNT && rfuCommandSessionActive_ &&
               hasEmeraldRfuProfile()) {
        // RFU ready handshakes invert with the clock direction. Use the
        // guest's selected direction (bit 0), not rfuAdapterClockMaster_:
        // the A5/A7 word still finishes a guest-clocked handshake, and the
        // A8 word still finishes an adapter-clocked one after role restoration.
        const bool guestClock = (value & 1) != 0;
        const bool guestReady = (value & 0x08) != 0;
        rfuSiLineHigh_ = guestClock ? !guestReady : guestReady;
    }
    const bool isMultiplayer = inMultiplayerMode();
    // Emerald DisableSerial/EnableSerial reset the guest parser with this
    // write while retaining multiplayer mode. Re-enter raw discovery and stop
    // autonomous child pulses; a mode-change check alone misses this boundary.
    if (remoteLinkEnabled_ && remoteBufferedProtocolRequested_ &&
        address == REG_SIOCNT && value == 0x2000)
        resetRemoteBufferedProtocol();
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
                const bool rfu32BitTransfer =
                    hasEmeraldRfuProfile() &&
                    (control & 0x1000) &&
                    (rfuLoginActive_ || rfuAdapterClockMaster_);
                if (rfu32BitTransfer) {
                    // Emerald's adapter-ID probe and RFU clock-role changes
                    // use the four 32-bit SIO baud selectors. In this mode
                    // SIOCNT bits 0-1 select baud; bit 0 is not an
                    // internal/external clock selector.
                    const uint32_t baud = RFU_NORMAL_BAUD[control & 3];
                    normalSerialCycles_ =
                        (32 * CPU_CLOCK + baud - 1) / baud;
                } else {
                    normalSerialCycles_ = (bits * CPU_CLOCK +
                        NORMAL_BAUD[(control >> 1) & 1] - 1) /
                        NORMAL_BAUD[(control >> 1) & 1];
                }
                // With no emulated Normal-mode peripheral, the SI line is
                // pulled high and the receiver shifts in one bits.
                if (!(control & 1) && !rfu32BitTransfer) {
                    if (rfuAdapterClockMaster_ && (control & 0x1000)) {
                        const uint32_t baud = RFU_NORMAL_BAUD[control & 3];
                        normalSerialCycles_ =
                            (32 * CPU_CLOCK + baud - 1) / baud;
                    } else {
                        normalSerialCycles_ = 0; // External clock has not arrived.
                    }
                }
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
    if (remoteLinkEnabled_ && remoteBufferedProtocolActive_) {
        remoteBufferedBaudIndex_ = static_cast<uint8_t>(value & 3);
        multiplayerWords_[1] = prepareRemoteBufferedTransferWord();
        remoteBufferedWordPending_ = true;
    }
    const uint32_t baud = MULTIPLAYER_BAUD[value & 3];
    // Two 16-bit words with one start and one stop bit each.
    multiplayerCycles_ = (36 * CPU_CLOCK + baud - 1) / baud;
    remotePeerWordReady_ = !remoteLinkEnabled_ ||
                           remoteBufferedProtocolActive_;
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
        if (remoteBufferedProtocolActive_) {
            remoteActiveSequence_ = ++remoteTransferSequence_;
            return;
        }
        if (!sendRemoteStart_)
            throw std::runtime_error("GBA remote link has no start sender");
        ++remoteTransferSequence_;
        remoteActiveSequence_ = ++remoteRawTransferSequence_;
        sendRemoteStart_(remoteActiveSequence_, multiplayerWords_[0],
                         static_cast<uint8_t>(value & 3),
                         remoteElapsedCycles());
    }
}

void GbaSystem::abortMultiplayerTransfer() {
    const bool hadPendingWork = multiplayerBusy_ ||
        !remotePendingStarts_.empty();
    if (remoteBufferedProtocolRequested_ && !remoteBufferedProtocolActive_ &&
        remoteLinkChild_ && remoteLinkConnected_ && sendRemoteWord_) {
        // A guest switching away from multiplayer cannot complete a discovery
        // transfer. Resolve it as an open slot instead of leaving the other
        // guest waiting for a reply that will never come.
        if (multiplayerBusy_ && !remoteReplySent_)
            sendRemoteWord_(remoteActiveSequence_, 0xFFFF);
        for (const auto &start : remotePendingStarts_)
            sendRemoteWord_(start.sequence, 0xFFFF);
    }
    remotePendingStarts_.clear();
    remotePredictedWords_.clear();
    remotePendingWords_.clear();
    remoteBufferedHandshakeStartsPending_ = 0;
    if (!remoteBufferedProtocolActive_)
        remotePendingDataBlocks_.clear();
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
    const bool completedBufferedHandshakeProbe =
        remoteLinkEnabled_ && remoteLinkChild_ &&
        remoteBufferedProtocolRequested_ && !remoteBufferedProtocolActive_ &&
        remoteBufferedHandshakeStartsPending_ > 0 &&
        multiplayerWords_[0] == 0x8FFF;
    const bool acceptedBufferedHandshakeProbe =
        completedBufferedHandshakeProbe && remoteBufferedSwitchAfterTransfer_;
    multiplayerCycles_ = 0;
    if (remoteLinkChild_ && remoteBufferedProtocolActive_)
        remoteChildAwaitingSend_ = true;
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
    if (completedBufferedHandshakeProbe &&
        !acceptedBufferedHandshakeProbe) {
        --remoteBufferedHandshakeStartsPending_;
        if (remoteBufferedHandshakeStartsPending_ == 0)
            remotePendingDataBlocks_.clear();
    }
}

void GbaSystem::tickMultiplayer(uint32_t cycles) {
    if (!multiplayerBusy_ || multiplayerCycles_ == 0)
        return;
    if (cycles < multiplayerCycles_) {
        multiplayerCycles_ -= cycles;
        return;
    }
    multiplayerCycles_ = 0;
    if (remoteBufferedProtocolActive_ && remoteBufferedWordPending_) {
        finishRemoteBufferedTransferWord();
        remoteBufferedWordPending_ = false;
    } else if (remoteLinkEnabled_ && remoteLinkChild_ && !remoteReplySent_) {
        sendRemoteWord_(remoteActiveSequence_, multiplayerWords_[1]);
        remoteReplySent_ = true;
    }
    if (!remoteLinkEnabled_ || remotePeerWordReady_) {
        completeMultiplayerTransfer();
        if (remoteBufferedSwitchAfterTransfer_)
            activateRemoteBufferedProtocol();
    }
}

void GbaSystem::answerEmeraldRfuRequest(
    const GbaRfuRequestDecoder::Request &request) {
    bool accepted = false;
    std::vector<uint32_t> response;
    const auto noParameters = [&request] { return request.payload.empty(); };
    const auto advertise = [this] {
        if (remoteRfuEnabled_ && sendRfuAdvertisement_)
            sendRfuAdvertisement_(rfuNextAdvertisementSequence_++,
                                 rfuLocalId_, rfuBroadcastData_);
    };
    const auto disconnect = [this](bool notifyPeer) {
        if (notifyPeer && remoteRfuEnabled_ && sendRfuDisconnect_)
            sendRfuDisconnect_();
        rfuDataChannel_.disconnect();
        rfuPendingTxData_.reset();
        rfuPendingTxClockChange_ = false;
        rfuGuestSyncRequired_ = true;
        rfuGroupedRounds_.clear();
        rfuCommandGroup_.reset();
        rfuPendingTxBuffered_ = false;
        rfuParentAttemptPending_ = false;
        rfuParentReplyReady_ = false;
        rfuDataAckPayloadPending_ = false;
        rfuRadioAwaitingReply_ = false;
        rfuRadioReplyClockChange_ = false;
        rfuConnected_ = false;
        rfuIsParent_ = false;
        rfuConnectionPending_ = false;
        rfuConnectedId_ = 0;
        rfuMasterSwitchPending_ = false;
        rfuAdapterClockMaster_ = false;
        rfuAdapterAwaitingAck_ = false;
        rfuAdapterEventCommand_ = 0;
        rfuDisconnectEventPending_ = false;
        rfuDisconnectPayloadPending_ = false;
    };

    switch (request.command) {
    case 0x10: // RESET_REQ; full state-reset semantics still need review.
        accepted = noParameters();
        break;
    case 0x11: // Signal levels are reported per connected client slot.
        accepted = noParameters();
        if (accepted) {
            const uint32_t level = rfuConnected_ ? 0xFFu : 0u;
            response.push_back(level << (rfuConnectedSlot_ * 8));
        }
        break;
    case 0x12: // Version status; report a known Wireless Adapter profile.
        accepted = noParameters();
        if (accepted)
            response.push_back(0x00830117);
        break;
    case 0x13: // System status.
        accepted = noParameters();
        if (accepted) {
            uint32_t state = 0;
            if (rfuSearchingChildren_)
                state = 2; // Serving an open room.
            else if (rfuConnectionPending_)
                state = 4;
            else if (rfuConnected_)
                state = rfuSearchingParents_ ? 5 : 1;
            else if (rfuSearchingParents_)
                state = 3;
            const uint32_t id = state == 0 ? 0u : rfuLocalId_;
            const uint32_t clientSlots =
                rfuConnected_ && !rfuSearchingChildren_
                    ? (1u << rfuConnectedSlot_) : 0u;
            response.push_back(id | (clientSlots << 16) | (state << 24));
        }
        break;
    case 0x16: // Game/player advertisement.
        accepted = request.payload.size() == rfuBroadcastData_.size();
        if (accepted) {
            std::copy(request.payload.begin(), request.payload.end(),
                      rfuBroadcastData_.begin());
            advertise();
        }
        break;
    case 0x17: // RFU system configuration.
        accepted = request.payload.size() == 1;
        if (accepted)
            rfuSystemConfig_ = request.payload.front();
        break;
    case 0x19: // Start child search.
        accepted = noParameters();
        if (accepted) {
            rfuSearchingChildren_ = true;
            advertise();
        }
        break;
    case 0x1A: // Poll child search.
        accepted = noParameters();
        if (accepted && rfuSearchingChildren_ && rfuConnected_) {
            response.push_back(static_cast<uint32_t>(rfuConnectedId_) |
                (static_cast<uint32_t>(rfuConnectedSlot_) << 16));
        }
        break;
    case 0x1D: // Poll parent search.
        accepted = noParameters();
        if (accepted && rfuSearchingParents_ &&
            rfuPeerAdvertisementKnown_) {
            response.reserve(7);
            // Search results carry the peer ID and its next available slot.
            response.push_back(static_cast<uint32_t>(rfuPeerId_));
            response.insert(response.end(), rfuPeerAdvertisement_.begin(),
                            rfuPeerAdvertisement_.end());
        }
        break;
    case 0x1B: // End child search.
        accepted = noParameters();
        if (accepted)
            rfuSearchingChildren_ = false;
        break;
    case 0x1C: // Start parent search.
        accepted = noParameters();
        if (accepted) {
            rfuSearchingParents_ = true;
            advertise();
        }
        break;
    case 0x1E: // End parent search.
        accepted = noParameters();
        if (accepted)
            rfuSearchingParents_ = false;
        break;
    case 0x1F: // Connect to the selected parent adapter.
        accepted = request.payload.size() == 1 && remoteRfuEnabled_ &&
            rfuPeerAdvertisementKnown_ &&
            static_cast<uint16_t>(request.payload.front()) == rfuPeerId_ &&
            !rfuConnected_ && !rfuConnectionPending_;
        if (accepted) {
            rfuConnectionPending_ = true;
            rfuPendingConnectionSequence_ = rfuNextConnectionSequence_++;
            sendRfuConnection_(rfuPendingConnectionSequence_, rfuLocalId_,
                               rfuPeerId_);
        }
        break;
    case 0x20: // Connection completion poll.
        accepted = noParameters();
        if (accepted) {
            const uint32_t result = rfuConnected_
                ? static_cast<uint32_t>(rfuLocalId_) |
                    (static_cast<uint32_t>(rfuConnectedSlot_) << 16)
                : 0x01000000;
            response.push_back(result);
        }
        break;
    case 0x21: // Finalize selected connection.
        accepted = noParameters();
        if (accepted)
            response.push_back(rfuConnected_
                ? static_cast<uint32_t>(rfuLocalId_) |
                    (static_cast<uint32_t>(rfuConnectedSlot_) << 16)
                : 0u);
        break;
    case 0x24: // Send link-layer data.
    case 0x25: // Send data and switch clock role.
        accepted = remoteRfuEnabled_ && rfuConnected_ && sendRfuData_ &&
            !(rfuCompletionMode_ == RfuCompletionMode::AsyncAdapter && rfuPendingTxData_) &&
            (!rfuIsParent_ || !rfuPendingTxData_ || !rfuPendingTxClockChange_) &&
            !request.payload.empty() &&
            request.payload.size() <= GbaRfuRequestDecoder::MAX_PAYLOAD_WORDS &&
            !(rfuIsParent_ && rfuCompletionMode_ != RfuCompletionMode::RemoteReply &&
              request.command == 0x25 &&
              ((rfuCompletionMode_ == RfuCompletionMode::GroupedCommands
                    ? (!rfuGroupedRounds_.empty() && rfuGroupedRounds_.back().clockChange &&
                       !rfuGroupedRounds_.back().buffered)
                    : (rfuRadioAwaitingReply_ && rfuRadioReplyClockChange_)) ||
               rfuParentReplyReady_ || rfuParentAttemptPending_ ||
               (rfuPendingTxData_ && rfuPendingTxClockChange_)));
        if (accepted) {
            const uint32_t txSequence = rfuNextDataSequence_;
            const auto admit = [this, &request](uint8_t bytes,
                                                const std::vector<uint32_t> &words) {
                logRfuDataDiagnostic("tx_write", rfuNextDataSequence_, bytes, words, rfuIsParent_);
                if (rfuCompletionMode_ == RfuCompletionMode::AsyncAdapter) {
                    if (dispatchRfuAsync(rfuNextDataSequence_, bytes, words, request.command == 0x25)) {
                        ++rfuNextDataSequence_;
                    } else {
                        rfuPendingTxData_ = RemoteRfuData{rfuNextDataSequence_, bytes, words};
                        rfuPendingTxClockChange_ = request.command == 0x25;
                    }
                    return true;
                }
                if (rfuIsParent_)
                    observeRfuSyncPayload(true, bytes, words);
                // A child send only writes its live TX register. The next
                // parent-triggered exchange selects and returns a snapshot.
                if (!rfuIsParent_)
                    return true;
                const bool grouped = rfuCompletionMode_ == RfuCompletionMode::GroupedCommands;
                const bool prefix = grouped && request.command == 0x25 &&
                    rfuCommandGroup_.append(bytes, words) ==
                        GbaRfuCommandGroup::Decision::BufferedPrefix;
                // Reserve the last RX/round slot for a peer-confirmed event.
                // Otherwise all slots can hold late prefix replies while the
                // guest waits for a new send's ACK before it can read 0x26.
                // That event lets the guest consume RX without dropping data.
                const bool buffered = prefix &&
                    rfuGroupedRounds_.size() + rfuDataChannel_.queuedMessages() + 1 <
                        GbaRfuDataChannel::MAX_QUEUED_MESSAGES;
                if (grouped) {
                    if (dispatchRfuParent(rfuNextDataSequence_, bytes, words,
                                          request.command == 0x25, buffered)) {
                        ++rfuNextDataSequence_;
                        return true;
                    }
                    rfuPendingTxData_ = RemoteRfuData{rfuNextDataSequence_, bytes, words};
                    rfuPendingTxClockChange_ = request.command == 0x25;
                    rfuPendingTxBuffered_ = buffered;
                    return true;
                }
                if (rfuPendingTxData_) {
                    rfuPendingTxData_->payloadBytes = bytes;
                    rfuPendingTxData_->frameWords = words;
                    rfuPendingTxClockChange_ = request.command == 0x25;
                    logRfuDiagnostic("RFU local TX register updated before exchange admission", true);
                    return true;
                }
                if (!rfuRadioAwaitingReply_) {
                    // Install state before invoking a sender: deterministic
                    // in-process peers may reply synchronously.
                    rfuRadioAwaitingReply_ = true;
                    rfuRadioReplyClockChange_ = request.command == 0x25;
                    if (sendRfuData_(rfuNextDataSequence_, bytes, words)) {
                        logRfuDataDiagnostic("tx_dispatch", rfuNextDataSequence_, bytes, words, true);
                        ++rfuNextDataSequence_;
                        return true;
                    }
                    rfuRadioAwaitingReply_ = false;
                    rfuRadioReplyClockChange_ = false;
                }
                rfuPendingTxData_ = RemoteRfuData{rfuNextDataSequence_, bytes, words};
                rfuPendingTxClockChange_ = request.command == 0x25;
                logRfuDiagnostic("RFU local TX buffered; command ACK remains local", true);
                return true;
            };
            // Parent TX stores the byte count directly. Child TX encodes
            // per-slot byte counts in bits 8..12; slot zero is this remote
            // parent's receive stream.
            const size_t frameBytes = rfuIsParent_
                ? request.payload.front()
                : ((request.payload.front() >> 8) & 0x1Fu);
            const size_t paddedFrameBytes =
                (request.payload.size() - 1) * sizeof(uint32_t);
            const bool ghostSend = rfuIsParent_ &&
                request.payload.size() == 1 && request.payload.front() == 1;
            if (ghostSend) {
                // A length-only parent send reuses the requested prefix of
                // the adapter's retained TX buffer. It is not an empty radio
                // packet. Emerald uses a one-byte ghost send to clock the
                // next child exchange when it has no new LLSF frame.
                const bool hasRetainedPrefix = rfuRetainedTxData_.has_value() &&
                    rfuRetainedTxData_->payloadBytes >= 1 &&
                    !rfuRetainedTxData_->frameWords.empty();
                // Compatibility initialization: the first idle send has no
                // retained bytes yet. Use a zero prefix, which Emerald's
                // parser ignores as shorter than an LLSF header. Hardware
                // power-on TX contents are not established by the ROM source.
                const uint32_t prefix = hasRetainedPrefix
                    ? rfuRetainedTxData_->frameWords.front() & 0xFFu : 0u;
                accepted = admit(1, {prefix});
                if (accepted) {
                    rfuRetainedTxData_ = RemoteRfuData{
                        txSequence, 1, {prefix}};
                }
            } else if (frameBytes <= paddedFrameBytes &&
                       frameBytes <= 127 &&
                       (rfuIsParent_ || frameBytes <= 31) &&
                       1 + (frameBytes + 3) / 4 <=
                           GbaRfuRequestDecoder::MAX_PAYLOAD_WORDS) {
                if (frameBytes == 0) {
                    accepted = admit(0, {});
                    if (!rfuIsParent_)
                        rfuRetainedTxData_.reset();
                } else {
                    const size_t wordCount = (frameBytes + 3) / 4;
                    std::vector<uint32_t> frameWords(
                        request.payload.begin() + 1,
                        request.payload.begin() + static_cast<ptrdiff_t>(
                            1 + wordCount));
                    accepted = admit(static_cast<uint8_t>(frameBytes), frameWords);
                    if (accepted) {
                        rfuRetainedTxData_ = RemoteRfuData{
                            txSequence,
                            static_cast<uint8_t>(frameBytes), frameWords};
                        const uint8_t firstFrameHeaderBytes =
                            rfuIsParent_ ? 3 : 2;
                        const uint8_t firstFramePayloadBytes =
                            static_cast<uint8_t>(frameWords.front() &
                                (rfuIsParent_ ? 0x7Fu : 0x1Fu));
                        char trace[192];
                        std::snprintf(trace, sizeof(trace),
                            "RFU data TX: seq=%u role=%s bytes=%zu meta=%08X "
                            "first=%08X llsf_header=%u first_len=%u",
                            txSequence,
                            rfuIsParent_ ? "parent" : "child", frameBytes,
                            request.payload.front(), frameWords.front(),
                            unsigned(firstFrameHeaderBytes),
                            unsigned(firstFramePayloadBytes));
                        logRfuDiagnostic(trace);
                    }
                }
            } else {
                accepted = false;
            }
            if (request.command == 0x25) {
                rfuMasterSwitchPending_ = accepted;
                if (accepted && rfuIsParent_ &&
                    rfuCompletionMode_ != RfuCompletionMode::RemoteReply)
                    rfuParentAttemptPending_ = true;
            }
        }
        break;
    case 0x26: // Receive queued link-layer data; empty polls keep later data.
        accepted = noParameters();
        if (accepted && rfuDataChannel_.hasReceived()) {
            if (rfuPendingConsumedSequence_)
                throw std::logic_error(
                    "RFU receive response already awaits serial completion");
            auto message = rfuDataChannel_.takeReceived();
            if (!message)
                throw std::logic_error("RFU receive queue changed unexpectedly");
            const uint32_t sequence = message->sequence;
            logRfuDataDiagnostic("rx_read", sequence, message->payloadBytes,
                                 message->frameWords, !rfuIsParent_);
            // DATA_RX_REQ returns an RfuPacket body, not a bare LLSF frame.
            // Emerald's parser reads the receive byte budget from data[0]
            // (packet offset 4) and starts LLSF data at packet offset 8. The
            // byte array includes the ACK word, so this is one metadata word
            // followed by frames, not two metadata words. Parent receives a
            // child per-slot count in bits 8..12; child receives the parent's
            // byte count in bits 0..6. This adapter uses peer slot zero.
            response.reserve(1 + message->frameWords.size());
            response.push_back(rfuIsParent_
                ? static_cast<uint32_t>(message->payloadBytes) << 8
                : message->payloadBytes);
            const uint32_t receiveMetadata = response.front();
            char trace[192];
            if (message->frameWords.empty()) {
                std::snprintf(trace, sizeof(trace),
                    "RFU data RX: seq=%u role=%s idle_slot=1 meta=%08X "
                    "queued=%zu", sequence,
                    rfuIsParent_ ? "parent" : "child", receiveMetadata,
                    rfuDataChannel_.queuedMessages());
            } else {
                const uint8_t firstFrameHeaderBytes =
                    rfuIsParent_ ? 2 : 3;
                const uint8_t firstFramePayloadBytes =
                        static_cast<uint8_t>(message->frameWords.front() &
                        (rfuIsParent_ ? 0x1Fu : 0x7Fu));
                std::snprintf(trace, sizeof(trace),
                    "RFU data RX: seq=%u role=%s bytes=%u meta=%08X "
                    "first=%08X llsf_header=%u first_len=%u queued=%zu",
                    sequence, rfuIsParent_ ? "parent" : "child",
                    unsigned(message->payloadBytes), receiveMetadata,
                    message->frameWords.front(),
                    unsigned(firstFrameHeaderBytes),
                    unsigned(firstFramePayloadBytes),
                    rfuDataChannel_.queuedMessages());
            }
            logRfuDiagnostic(trace);
            response.insert(response.end(), message->frameWords.begin(),
                            message->frameWords.end());
            rfuPendingConsumedSequence_ = sequence;
        }
        break;
    case 0x27: // MS_CHANGE_REQ switches the RFU port into adapter-clocked mode.
        accepted = noParameters();
        if (accepted)
            rfuMasterSwitchPending_ = true;
        break;
    case 0x37: // Resume an outstanding parent exchange, without resending it.
        accepted = noParameters() && remoteRfuEnabled_ && rfuConnected_ &&
            rfuIsParent_ && rfuCompletionMode_ != RfuCompletionMode::RemoteReply &&
            (rfuRadioAwaitingReply_ || rfuParentReplyReady_ ||
             (rfuPendingTxData_ && rfuPendingTxClockChange_));
        if (accepted) {
            rfuParentAttemptPending_ = true;
            rfuMasterSwitchPending_ = true;
        }
        break;
    case 0x30: // Disconnect.
        accepted = request.payload.size() == 1;
        if (accepted) {
            logRfuDisconnectDiagnostic(request.command);
            disconnect(rfuConnected_ || rfuConnectionPending_);
        }
        break;
    case 0x3D: // STOP_MODE_REQ; Emerald also issues this during InitRFU.
        accepted = noParameters();
        // The cartridge sends this while initialising the RFU before it
        // configures or searches for peers. Treating it as a LAN disconnect
        // tears down the transport before the first advertisement. It stops
        // the guest's current adapter activity; it does not end the netplay
        // peer session.
        if (accepted) {
            rfuSearchingChildren_ = false;
            rfuSearchingParents_ = false;
        }
        break;
    default:
        break;
    }
    char trace[256];
    std::snprintf(trace, sizeof(trace),
                  "RFU command: id=%02X payload_words=%zu result=%s response_words=%zu "
                  "response0=%08X children=%u parents=%u connected=%u pending=%u",
                  request.command, request.payload.size(),
                  accepted ? "accepted" : "rejected", response.size(),
                  response.empty() ? 0u : response.front(),
                  unsigned(rfuSearchingChildren_), unsigned(rfuSearchingParents_),
                  unsigned(rfuConnected_), unsigned(rfuConnectionPending_));
    logRfuDiagnostic(trace, request.command != 0x1D &&
        request.command != 0x1A && request.command != 0x11 &&
        request.command != 0x13);
    rfuCommandSession_.respond(accepted, std::move(response), 1);
}

void GbaSystem::servicePendingEmeraldRfuTransmit() {
    if (!rfuPendingTxData_ || !remoteRfuEnabled_ || !rfuConnected_ ||
        (rfuIsParent_ && !rfuParentCanDispatch()))
        return;
    const auto packet = *rfuPendingTxData_;
    if (rfuCompletionMode_ == RfuCompletionMode::AsyncAdapter) {
        if (!dispatchRfuAsync(packet.sequence, packet.payloadBytes, packet.frameWords,
                               rfuPendingTxClockChange_)) return;
        ++rfuNextDataSequence_;
        rfuPendingTxData_.reset(); rfuPendingTxClockChange_ = false;
        return;
    }
    if (rfuIsParent_ && rfuCompletionMode_ == RfuCompletionMode::GroupedCommands) {
        if (!dispatchRfuParent(packet.sequence, packet.payloadBytes, packet.frameWords,
                               rfuPendingTxClockChange_, rfuPendingTxBuffered_)) return;
        ++rfuNextDataSequence_;
        rfuPendingTxData_.reset(); rfuPendingTxClockChange_ = false;
        rfuPendingTxBuffered_ = false;
        return;
    }
    if (rfuIsParent_) {
        rfuRadioAwaitingReply_ = true;
        rfuRadioReplyClockChange_ = rfuPendingTxClockChange_;
    }
    if (!sendRfuData_(packet.sequence, packet.payloadBytes, packet.frameWords)) {
        if (rfuIsParent_) {
            rfuRadioAwaitingReply_ = false;
            rfuRadioReplyClockChange_ = false;
        }
        return;
    }
    logRfuDataDiagnostic("tx_dispatch", packet.sequence, packet.payloadBytes,
                         packet.frameWords, rfuIsParent_);
    ++rfuNextDataSequence_;
    char trace[128];
    std::snprintf(trace, sizeof(trace), "RFU local TX dispatched: seq=%u", packet.sequence);
    logRfuDiagnostic(trace, true);
    rfuPendingTxData_.reset();
    rfuPendingTxClockChange_ = false;
}

void GbaSystem::tickNormalSerial(uint32_t cycles) {
    if (!normalSerialBusy_ || normalSerialCycles_ == 0)
        return;
    if (rfuAdapterClockMaster_ && !rfuAdapterAwaitingAck_ &&
        remoteRfuEnabled_ && rfuConnected_ &&
        (loadIo16(REG_SIOCNT) & 1) == 0 &&
        rfuPendingRadioEvents_ == 0 &&
        !rfuParentPollReady()) {
        // Wait for a real cycle, or an explicitly armed local parent poll.
        // Merely scheduling a child TX cannot create an adapter event.
        return;
    }
    if (cycles < normalSerialCycles_) {
        normalSerialCycles_ -= cycles;
        return;
    }
    normalSerialCycles_ = 0;
    normalSerialBusy_ = false;
    const uint16_t control = read16Impl(REG_SIOCNT);
    storeIo16(REG_SIOCNT, control & ~SIO_START);
    if ((control & 0x1000) && (control & 1) == 0 &&
        rfuAdapterClockMaster_ && hasEmeraldRfuProfile()) {
        // RFU adapter events use the clock-slave SIO path. A completed parent
        // radio cycle emits 0x28; a peer loss while the adapter owns the clock
        // emits 0x29 and its packed slot/reason word. ACK completion returns
        // clock ownership to Emerald; this models guest-visible events, not
        // RFU radio timing.
        const uint32_t guestWord = static_cast<uint32_t>(
            read16Impl(REG_SIOMULTI0)) |
            (static_cast<uint32_t>(read16Impl(REG_SIOMULTI0 + 2)) << 16);
        uint32_t adapterWord = 0x80000000;
        const bool startingEvent = !rfuAdapterAwaitingAck_;
        if (startingEvent) {
            if (rfuPendingRadioEvents_ > 0)
                --rfuPendingRadioEvents_;
            if (rfuDisconnectEventPending_) {
                rfuAdapterEventCommand_ = 0x29;
                rfuDisconnectEventPending_ = false;
                rfuDisconnectPayloadPending_ = true;
                adapterWord = makeGbaRfuCommandHeader(0x29, 1);
                logRfuDiagnostic(
                    "RFU adapter clocked DISCONNECTED_AND_CHANGE_REQ (0x29)");
            } else {
                rfuAdapterEventCommand_ = 0x28;
                // Compatibility: finish a local attempt without inventing a
                // peer ACK. Emerald's empty 0x28 implies all slots succeeded;
                // an explicit mask of zero instead selects its 0x37 retry
                // path and releases the main loop while WAN delivery waits.
                // See docs/gba-rfu-protocol.md: Local parent completion polls.
                const bool localPoll = rfuIsParent_ &&
                    rfuCompletionMode_ != RfuCompletionMode::RemoteReply &&
                    rfuParentAttemptPending_;
                rfuDataAckPayloadPending_ = localPoll;
                rfuDataAckMask_ = rfuParentReplyReady_
                    ? static_cast<uint8_t>(1u << rfuConnectedSlot_) : 0;
                if (localPoll) {
                    rfuParentAttemptPending_ = false;
                    rfuParentReplyReady_ = false;
                }
                adapterWord = makeGbaRfuCommandHeader(0x28, localPoll ? 1 : 0);
                logRfuDiagnostic(
                    "RFU adapter clocked DATA_READY_AND_CHANGE_REQ (0x28)", true);
            }
            rfuAdapterAwaitingAck_ = true;
        } else if (rfuDisconnectPayloadPending_) {
            // 0x29 has one request word: slot bitmask followed by reason.
            // Emerald sends 0xA9 after this word on the next adapter clock.
            adapterWord = static_cast<uint32_t>(rfuDisconnectEventSlotMask_) |
                (static_cast<uint32_t>(rfuDisconnectEventReason_) << 8);
            rfuDisconnectPayloadPending_ = false;
        } else if (rfuDataAckPayloadPending_) {
            // Snapshot at the event header: a reply arriving between header
            // and body belongs to the next poll, not this half-sent event.
            adapterWord = rfuDataAckMask_;
            rfuDataAckPayloadPending_ = false;
        } else {
            const uint32_t expectedAck = makeGbaRfuResponseHeader(
                rfuAdapterEventCommand_, 0);
            // SIO is full duplex. Preserve the observed next-word ACK path;
            // the simultaneous 0x28/0xA8 case remains a compatibility path.
            if (guestWord == expectedAck) {
                rfuAdapterClockMaster_ = false;
                rfuAdapterAwaitingAck_ = false;
                rfuAdapterEventCommand_ = 0;
                rfuPendingRadioEvents_ = 0;
                logRfuDiagnostic(
                    "RFU adapter event ACK received; guest SIO master role restored", true);
            } else {
                adapterWord = 0xFFFFFFFF;
                logRfuDiagnostic(
                    "RFU adapter event received an invalid command ACK", true);
            }
        }
        if (startingEvent && rfuAdapterEventCommand_ == 0x28 &&
            !rfuDataAckPayloadPending_ && guestWord == makeGbaRfuResponseHeader(0x28, 0)) {
            // Retained as a compatibility allowance for the simultaneous
            // full-duplex word shape; normal decompiled flow ACKs next.
            rfuAdapterClockMaster_ = false;
            rfuAdapterAwaitingAck_ = false;
            rfuAdapterEventCommand_ = 0;
            rfuPendingRadioEvents_ = 0;
            logRfuDiagnostic(
                "RFU adapter event ACK received in the header transfer", true);
        }
        char trace[192];
        std::snprintf(trace, sizeof(trace),
                      "RFU adapter word guest=%08X adapter=%08X clock=%s",
                      guestWord, adapterWord,
                      rfuAdapterClockMaster_ ? "adapter" : "guest");
        logRfuDiagnostic(trace, true);
        // The slave IRQ first waits for SI low, then raises SO and waits
        // for SI high. This final external handshake also applies to A8.
        rfuSiLineHigh_ = false;
        storeIo16(REG_SIOMULTI0, static_cast<uint16_t>(adapterWord));
        storeIo16(REG_SIOMULTI0 + 2,
                  static_cast<uint16_t>(adapterWord >> 16));
        if (control & SIO_IRQ_ENABLE)
            storeIo16(REG_IF, read16Impl(REG_IF) | IRQ_SERIAL);
        return;
    }
    if ((control & 0x1000) && rfuLoginActive_ &&
        hasEmeraldRfuProfile() && !rfuLogin_.complete()) {
        // Emerald's adapter login is an RFU-specific 32-bit SIO
        // exchange. This clean-room endpoint reproduces the public NINTENDO
        // challenge sequence; command service begins only after login.
        const uint32_t guestWord = static_cast<uint32_t>(
            read16Impl(REG_SIOMULTI0)) |
            (static_cast<uint32_t>(read16Impl(REG_SIOMULTI0 + 2)) << 16);
        const uint32_t adapterWord = rfuLogin_.exchangeWord(guestWord);
        ++rfuTransferCount_;
        char trace[160];
        std::snprintf(trace, sizeof(trace),
                      "RFU login word=%u guest=%08X adapter=%08X",
                      rfuLogin_.transferCount(), guestWord, adapterWord);
        logRfuDiagnostic(trace);
        storeIo16(REG_SIOMULTI0, static_cast<uint16_t>(adapterWord));
        storeIo16(REG_SIOMULTI0 + 2,
                  static_cast<uint16_t>(adapterWord >> 16));
        if (rfuLogin_.complete()) {
            rfuLoginActive_ = false;
            rfuCommandSessionActive_ = true;
            rfuSiLineHigh_ = true;
        }
    } else if ((control & 0x1000) && rfuCommandSessionActive_ &&
               hasEmeraldRfuProfile()) {
        const uint32_t guestWord = static_cast<uint32_t>(
            read16Impl(REG_SIOMULTI0)) |
            (static_cast<uint32_t>(read16Impl(REG_SIOMULTI0 + 2)) << 16);
        auto exchange = rfuCommandSession_.exchangeWord(guestWord);
        ++rfuTransferCount_;
        if (exchange.protocolError) {
            char trace[160];
            std::snprintf(trace, sizeof(trace),
                          "RFU protocol error at word=%u guest=%08X",
                          rfuTransferCount_, guestWord);
            logRfuDiagnostic(trace);
            rfuCommandSession_.reset();
            rfuPendingConsumedSequence_.reset();
            exchange.adapterWord = 0xFFFFFFFF;
        } else if (exchange.requestComplete) {
            answerEmeraldRfuRequest(exchange.request);
        }
        if (exchange.responseComplete && rfuPendingConsumedSequence_) {
            const uint32_t sequence = *rfuPendingConsumedSequence_;
            rfuPendingConsumedSequence_.reset();
            char trace[128];
            std::snprintf(trace, sizeof(trace),
                          "RFU data consumed: seq=%u response_complete=1",
                          sequence);
            logRfuDiagnostic(trace);
        }
        if (rfuMasterSwitchPending_ &&
            (exchange.adapterWord == makeGbaRfuResponseHeader(0x27, 0) ||
             exchange.adapterWord == makeGbaRfuResponseHeader(0x25, 0) ||
             exchange.adapterWord == makeGbaRfuResponseHeader(0x37, 0))) {
            rfuMasterSwitchPending_ = false;
            rfuAdapterClockMaster_ = true;
            rfuAdapterAwaitingAck_ = false;
            logRfuDiagnostic(
                "RFU change-request ACK complete; adapter now clocks SIO", true);
        }
        char trace[160];
        std::snprintf(trace, sizeof(trace),
                      "RFU command word=%u guest=%08X adapter=%08X",
                      rfuTransferCount_, guestWord, exchange.adapterWord);
        logRfuDiagnostic(trace);
        storeIo16(REG_SIOMULTI0,
                  static_cast<uint16_t>(exchange.adapterWord));
        storeIo16(REG_SIOMULTI0 + 2,
                  static_cast<uint16_t>(exchange.adapterWord >> 16));
        rfuSiLineHigh_ = true;
    } else if (control & 0x1000) {
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
    servicePendingEmeraldRfuTransmit();
    initializeScanlineOam();
    uint32_t elapsed = 0;
    uint32_t nextNetworkPoll = 0;
    while (remoteFrameCycles_ < CYCLES_PER_FRAME) {
        const bool rfuPump = remoteRfuEnabled_ && rfuConnected_ && rfuNetworkPump_;
        const bool cablePump = remoteLinkEnabled_ && remoteLinkConnected_ &&
            remoteBufferedProtocolRequested_ && remoteEventPump_;
        if ((rfuPump || cablePump) && remoteFrameCycles_ >= nextNetworkPoll) {
            nextNetworkPoll += CYCLES_PER_FRAME / 8;
            // A receive callback can disconnect and clear the installed pump.
            // Keep this invocation alive and re-check state after it returns.
            const auto pump = rfuPump ? rfuNetworkPump_ : remoteEventPump_;
            pump();
            servicePendingEmeraldRfuTransmit();
        }
        if (remoteLinkEnabled_ && remoteBufferedProtocolRequested_ &&
            !servicePendingRemoteDataBlock()) {
            remoteFrameStalled_ = true;
            return {true, elapsed};
        }
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
                    remoteBufferedSwitchAfterTransfer_ =
                        remoteBufferedProtocolRequested_ && !remoteLinkChild_ &&
                        multiplayerWords_[0] == 0x8FFF && *predicted == 0xB9A0;
                    completeMultiplayerTransfer();
                    if (remoteBufferedSwitchAfterTransfer_)
                        activateRemoteBufferedProtocol();
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
    if (remoteRfuEnabled_) {
        ++rfuGuestFrameCount_;
        logRfuFrameDiagnostic();
    }
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
