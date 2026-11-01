#include "include/cartridge.hpp"
#include "include/state_hash.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace LOTUSGB {
namespace {

constexpr size_t ROM_BANK_SIZE = 0x4000;
constexpr size_t RAM_BANK_SIZE = 0x2000;
constexpr size_t MINIMUM_BANKED_ROM_SIZE = 2 * ROM_BANK_SIZE;
constexpr size_t MAXIMUM_MBC2_ROM_SIZE = 16 * ROM_BANK_SIZE;
constexpr size_t MAXIMUM_MBC_ROM_SIZE = 2 * 1024 * 1024;
constexpr size_t MAXIMUM_MBC30_ROM_SIZE = 4 * 1024 * 1024;
constexpr size_t MAXIMUM_MBC5_ROM_SIZE = 8 * 1024 * 1024;
constexpr size_t MBC2_RAM_SIZE = 0x0200;
constexpr size_t MBC3_RTC_STORAGE_SIZE = 24;
constexpr size_t CARTRIDGE_TYPE_OFFSET = 0x0147;
constexpr size_t RAM_SIZE_OFFSET = 0x0149;
// Opaque libretro RTC payload: signature/version, live and latched registers,
// latch-edge state, and the last Unix timestamp in seconds.
constexpr uint8_t MBC3_RTC_MAGIC[] = {'L', 'G', 'R', 'T'};
constexpr uint8_t MBC3_RTC_VERSION = 1;
constexpr size_t RTC_LIVE_REGISTERS_OFFSET = 5;
constexpr size_t RTC_LATCHED_REGISTERS_OFFSET = 10;
constexpr size_t RTC_LAST_LATCH_VALUE_OFFSET = 15;
constexpr size_t RTC_LAST_UNIX_TIME_OFFSET = 16;

int64_t getUnixTimeNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

size_t getMbc1RamSize(uint8_t sizeCode) {
    switch (sizeCode) {
    case 0x00:
        return 0;
    case 0x02:
        return 0x2000;
    case 0x03:
        return 0x8000;
    default:
        throw std::invalid_argument("unsupported MBC1 RAM size");
    }
}

size_t getMbc3RamSize(uint8_t sizeCode) {
    switch (sizeCode) {
    case 0x00:
        return 0;
    case 0x01:
        return 0x0800;
    case 0x02:
        return 0x2000;
    case 0x03:
        return 0x8000;
    case 0x05:
        return 0x10000;
    default:
        throw std::invalid_argument("unsupported MBC3/MBC30 RAM size");
    }
}

size_t getMbc5RamSize(uint8_t sizeCode, bool rumble) {
    switch (sizeCode) {
    case 0x00:
        return 0;
    case 0x02:
        return 0x2000;
    case 0x03:
        return 0x8000;
    case 0x04:
        if (!rumble)
            return 0x20000;
        break;
    case 0x05:
        if (!rumble)
            return 0x10000;
        break;
    default:
        break;
    }
    throw std::invalid_argument("unsupported MBC5 RAM size");
}

}

Cartridge::Cartridge(std::vector<uint8_t> rom,
                     std::shared_ptr<std::vector<uint8_t>> saveRam,
                     std::shared_ptr<std::vector<uint8_t>> saveRtc,
                     RtcNowFunction rtcNow)
    : Cartridge(std::move(rom), std::move(saveRam), std::move(saveRtc),
                std::move(rtcNow), nullptr) {}

Cartridge::Cartridge(std::vector<uint8_t> rom,
                     std::shared_ptr<std::vector<uint8_t>> saveRam,
                     std::shared_ptr<std::vector<uint8_t>> saveRtc,
                     RtcNowFunction rtcNow,
                     const std::array<uint8_t, 32> *knownRomHash)
    : rom(std::move(rom)), rtcNow(std::move(rtcNow)) {
    if (this->rom.empty())
        throw std::invalid_argument("cartridge ROM cannot be empty");
    romHash = knownRomHash
                  ? *knownRomHash
                  : StateInternal::sha256(this->rom.data(), this->rom.size());
    if (!this->rtcNow)
        this->rtcNow = getUnixTimeNow;

    const uint8_t type = this->rom.size() > CARTRIDGE_TYPE_OFFSET
                             ? this->rom[CARTRIDGE_TYPE_OFFSET]
                             : 0x00;
    switch (type) {
    case 0x00:
        if (this->rom.size() > 0x8000)
            throw std::invalid_argument("ROM-only cartridge exceeds 32 KiB");
        return;
    case 0x05:
    case 0x06:
        if (this->rom.size() < MINIMUM_BANKED_ROM_SIZE ||
            this->rom.size() > MAXIMUM_MBC2_ROM_SIZE ||
            this->rom.size() % ROM_BANK_SIZE != 0)
            throw std::invalid_argument("MBC2 cartridge must contain 32 KiB to 256 KiB of banked ROM");
        mapper = Mapper::Mbc2;
        battery = type == 0x06;
        if (battery && saveRam) {
            if (saveRam->size() != MBC2_RAM_SIZE)
                throw std::invalid_argument("MBC2 save RAM size must be 512 bytes");
            externalRam = std::move(saveRam);
        } else {
            externalRam = std::make_shared<std::vector<uint8_t>>(
                MBC2_RAM_SIZE, 0xFF);
        }
        return;
    case 0x01:
    case 0x02:
    case 0x03:
        if (this->rom.size() < MINIMUM_BANKED_ROM_SIZE ||
            this->rom.size() > MAXIMUM_MBC_ROM_SIZE ||
            this->rom.size() % ROM_BANK_SIZE != 0)
            throw std::invalid_argument("MBC1 cartridge must contain 32 KiB to 2 MiB of banked ROM");
        mapper = Mapper::Mbc1;
        battery = type == 0x03;
        if (type == 0x02 || type == 0x03) {
            const uint8_t sizeCode = this->rom.size() > RAM_SIZE_OFFSET
                                         ? this->rom[RAM_SIZE_OFFSET]
                                         : 0x00;
            const size_t ramSize = getMbc1RamSize(sizeCode);
            if (battery && saveRam) {
                if (saveRam->size() != ramSize)
                    throw std::invalid_argument("MBC1 save RAM size does not match the cartridge header");
                externalRam = std::move(saveRam);
            } else {
                externalRam = std::make_shared<std::vector<uint8_t>>(
                    ramSize, 0xFF);
            }
        }
        return;
    case 0x0F:
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
        if (this->rom.size() < MINIMUM_BANKED_ROM_SIZE ||
            this->rom.size() > MAXIMUM_MBC30_ROM_SIZE ||
            this->rom.size() % ROM_BANK_SIZE != 0)
            throw std::invalid_argument("MBC3/MBC30 cartridge must contain 32 KiB to 4 MiB of banked ROM");
        mapper = Mapper::Mbc3;
        // The header has no separate MBC30 type. Expanded geometry selects
        // the variant; no game-title or ROM-hash special case is involved.
        mbc30 = this->rom.size() > MAXIMUM_MBC_ROM_SIZE;
        mbc3HasRtc = type == 0x0F || type == 0x10;
        battery = mbc3HasRtc || type == 0x13;
        if (type == 0x10 || type == 0x12 || type == 0x13) {
            const uint8_t sizeCode = this->rom.size() > RAM_SIZE_OFFSET
                                         ? this->rom[RAM_SIZE_OFFSET]
                                         : 0x00;
            const size_t ramSize = getMbc3RamSize(sizeCode);
            mbc30 = mbc30 || ramSize > 0x8000;
            if (battery && saveRam) {
                if (saveRam->size() != ramSize)
                    throw std::invalid_argument("MBC3 save RAM size does not match the cartridge header");
                externalRam = std::move(saveRam);
            } else {
                externalRam = std::make_shared<std::vector<uint8_t>>(
                    ramSize, 0xFF);
            }
        }
        if (mbc3HasRtc) {
            const bool hasRestoredRtc = static_cast<bool>(saveRtc);
            rtcStorage = std::move(saveRtc);
            if (!rtcStorage)
                rtcStorage = std::make_shared<std::vector<uint8_t>>(
                    MBC3_RTC_STORAGE_SIZE, 0);
            if (rtcStorage->size() != MBC3_RTC_STORAGE_SIZE)
                throw std::invalid_argument("MBC3 RTC save data must be 24 bytes");
            if (!hasRestoredRtc)
                initializeRtcStorage();
        }
        return;
    case 0x19:
    case 0x1A:
    case 0x1B:
    case 0x1C:
    case 0x1D:
    case 0x1E: {
        if (this->rom.size() < MINIMUM_BANKED_ROM_SIZE ||
            this->rom.size() > MAXIMUM_MBC5_ROM_SIZE ||
            this->rom.size() % ROM_BANK_SIZE != 0)
            throw std::invalid_argument("MBC5 cartridge must contain 32 KiB to 8 MiB of banked ROM");
        mapper = Mapper::Mbc5;
        mbc5HasRumble = type >= 0x1C;
        battery = type == 0x1B || type == 0x1E;
        const bool hasRam = type == 0x1A || type == 0x1B ||
                            type == 0x1D || type == 0x1E;
        if (hasRam) {
            const uint8_t sizeCode = this->rom.size() > RAM_SIZE_OFFSET
                                         ? this->rom[RAM_SIZE_OFFSET]
                                         : 0x00;
            const size_t ramSize = getMbc5RamSize(sizeCode, mbc5HasRumble);
            if (battery && saveRam) {
                if (saveRam->size() != ramSize)
                    throw std::invalid_argument("MBC5 save RAM size does not match the cartridge header");
                externalRam = std::move(saveRam);
            } else {
                externalRam = std::make_shared<std::vector<uint8_t>>(
                    ramSize, 0xFF);
            }
        }
        return;
    }
    default:
        throw std::invalid_argument("unsupported Game Boy cartridge type");
    }
}

uint8_t Cartridge::read(uint16_t address) {
    if (address <= 0x7FFF)
        return readRom(address);
    if (address >= 0xA000 && address <= 0xBFFF)
        return readExternalRam(address);
    return 0xFF;
}

void Cartridge::write(uint16_t address, uint8_t value) {
    if (mapper == Mapper::RomOnly)
        return;

    if (mapper == Mapper::Mbc5) {
        if (address <= 0x1FFF) {
            ramEnabled = externalRam && !externalRam->empty() &&
                         (value & 0x0F) == 0x0A;
        } else if (address <= 0x2FFF) {
            mbc5RomBank = (mbc5RomBank & 0x0100) | value;
        } else if (address <= 0x3FFF) {
            mbc5RomBank = (mbc5RomBank & 0x00FF) |
                          ((value & 0x01) << 8);
        } else if (address <= 0x5FFF) {
            // Rumble cartridges use bit 3 for the motor and only bits 0-2
            // for RAM banking. The motor signal is not exposed by this core.
            mbc5RamBank = value & (mbc5HasRumble ? 0x07 : 0x0F);
        } else if (address >= 0xA000 && address <= 0xBFFF) {
            writeExternalRam(address, value);
        }
    } else if (mapper == Mapper::Mbc2 && address <= 0x3FFF) {
        if ((address & 0x0100) == 0) {
            ramEnabled = (value & 0x0F) == 0x0A;
        } else {
            mbc2RomBank = value & 0x0F;
            if (mbc2RomBank == 0)
                mbc2RomBank = 1;
        }
    } else if (address <= 0x1FFF) {
        const bool hasAccessibleStorage =
            (externalRam && !externalRam->empty()) ||
            (mapper == Mapper::Mbc3 && mbc3HasRtc);
        ramEnabled = hasAccessibleStorage && (value & 0x0F) == 0x0A;
    } else if (address <= 0x3FFF) {
        if (mapper == Mapper::Mbc1) {
            romBankLow = value & 0x1F;
            if (romBankLow == 0)
                romBankLow = 1;
        } else if (mapper == Mapper::Mbc3) {
            mbc3RomBank = value & (mbc30 ? 0xFF : 0x7F);
            if (mbc3RomBank == 0)
                mbc3RomBank = 1;
        }
    } else if (address <= 0x5FFF) {
        if (mapper == Mapper::Mbc1)
            bankHigh = value & 0x03;
        else if (mapper == Mapper::Mbc3)
            mbc3RamBank = value;
    } else if (address <= 0x7FFF) {
        if (mapper == Mapper::Mbc1)
            ramBankingMode = (value & 0x01) != 0;
        else if (mapper == Mapper::Mbc3 && mbc3HasRtc)
            latchRtc(value);
    } else if (address >= 0xA000 && address <= 0xBFFF) {
        writeExternalRam(address, value);
    }
}

bool Cartridge::hasBattery() const {
    return battery;
}

uint8_t *Cartridge::getExternalRamData() {
    return !externalRam || externalRam->empty() ? nullptr : externalRam->data();
}

const uint8_t *Cartridge::getExternalRamData() const {
    return !externalRam || externalRam->empty() ? nullptr : externalRam->data();
}

size_t Cartridge::getExternalRamSize() const {
    return externalRam ? externalRam->size() : 0;
}

std::shared_ptr<std::vector<uint8_t>> Cartridge::getBatteryRamStorage() const {
    return battery ? externalRam : nullptr;
}

std::shared_ptr<std::vector<uint8_t>> Cartridge::getBatteryRtcStorage() const {
    return battery ? rtcStorage : nullptr;
}

void Cartridge::initializeRtcStorage() {
    mbc3RtcRegisters.fill(0);
    mbc3RtcLatchedRegisters = mbc3RtcRegisters;
    mbc3RtcLastUnixTime = std::max<int64_t>(0, rtcNow());
    mbc3RtcLastLatchValue = 0xFF;
    mbc3RtcLoaded = false;
    saveRtcState();
    // A frontend can restore its .rtc block after retro_load_game returns.
    // Parse the buffer on the first emulated RTC access so that restoration is
    // observed even though this object was constructed before that callback.
}

void Cartridge::ensureRtcLoaded() {
    if (mbc3RtcLoaded)
        return;

    const bool hasValidStorage =
        rtcStorage && rtcStorage->size() == MBC3_RTC_STORAGE_SIZE &&
        std::equal(std::begin(MBC3_RTC_MAGIC), std::end(MBC3_RTC_MAGIC),
                   rtcStorage->begin()) &&
        (*rtcStorage)[4] == MBC3_RTC_VERSION;
    if (hasValidStorage) {
        std::copy_n(rtcStorage->begin() + RTC_LIVE_REGISTERS_OFFSET,
                    mbc3RtcRegisters.size(), mbc3RtcRegisters.begin());
        std::copy_n(rtcStorage->begin() + RTC_LATCHED_REGISTERS_OFFSET,
                    mbc3RtcLatchedRegisters.size(),
                    mbc3RtcLatchedRegisters.begin());
        mbc3RtcRegisters[0] &= 0x3F;
        mbc3RtcRegisters[1] &= 0x3F;
        mbc3RtcRegisters[2] &= 0x1F;
        mbc3RtcRegisters[4] &= 0xC1;
        mbc3RtcLatchedRegisters[0] &= 0x3F;
        mbc3RtcLatchedRegisters[1] &= 0x3F;
        mbc3RtcLatchedRegisters[2] &= 0x1F;
        mbc3RtcLatchedRegisters[4] &= 0xC1;
        mbc3RtcLastLatchValue =
            (*rtcStorage)[RTC_LAST_LATCH_VALUE_OFFSET];

        uint64_t storedTime = 0;
        for (size_t i = 0; i < sizeof(storedTime); ++i)
            storedTime |= static_cast<uint64_t>(
                              (*rtcStorage)[RTC_LAST_UNIX_TIME_OFFSET + i])
                          << (i * 8);
        if (storedTime <=
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            mbc3RtcLastUnixTime = static_cast<int64_t>(storedTime);
            mbc3RtcLoaded = true;
        }
    }

    if (!mbc3RtcLoaded) {
        mbc3RtcRegisters.fill(0);
        mbc3RtcLatchedRegisters = mbc3RtcRegisters;
        mbc3RtcLastUnixTime = std::max<int64_t>(0, rtcNow());
        mbc3RtcLastLatchValue = 0xFF;
        mbc3RtcLoaded = true;
    }
    saveRtcState();
}

void Cartridge::syncRtcToNow() {
    ensureRtcLoaded();
    const int64_t now = std::max<int64_t>(0, rtcNow());
    if (now < mbc3RtcLastUnixTime) {
        // A wall clock correction must not subtract time from the emulated
        // clock. Rebase the timestamp and resume from the corrected clock.
        mbc3RtcLastUnixTime = now;
        saveRtcState();
        return;
    }

    const uint64_t elapsed = static_cast<uint64_t>(now - mbc3RtcLastUnixTime);
    mbc3RtcLastUnixTime = now;
    if (mbc3RtcRegisters[4] & 0x40) {
        saveRtcState();
        return;
    }
    if (elapsed == 0)
        return;

    const uint64_t secondsOfDay =
        mbc3RtcRegisters[0] + 60ULL * mbc3RtcRegisters[1] +
        3600ULL * mbc3RtcRegisters[2] + elapsed;
    const uint64_t daysElapsed = secondsOfDay / 86400;
    uint64_t days = mbc3RtcRegisters[3] |
                    (static_cast<uint64_t>(mbc3RtcRegisters[4] & 0x01)
                     << 8);
    days += daysElapsed;
    if (days >= 512)
        mbc3RtcRegisters[4] |= 0x80;
    days %= 512;

    const uint64_t secondsWithinDay = secondsOfDay % 86400;
    mbc3RtcRegisters[0] = static_cast<uint8_t>(secondsWithinDay % 60);
    mbc3RtcRegisters[1] =
        static_cast<uint8_t>((secondsWithinDay / 60) % 60);
    mbc3RtcRegisters[2] =
        static_cast<uint8_t>(secondsWithinDay / 3600);
    mbc3RtcRegisters[3] = static_cast<uint8_t>(days & 0xFF);
    mbc3RtcRegisters[4] = static_cast<uint8_t>(
        (mbc3RtcRegisters[4] & 0xC0) | ((days >> 8) & 0x01));
    saveRtcState();
}

void Cartridge::saveRtcState() {
    if (!rtcStorage || rtcStorage->size() != MBC3_RTC_STORAGE_SIZE)
        return;

    std::copy(std::begin(MBC3_RTC_MAGIC), std::end(MBC3_RTC_MAGIC),
              rtcStorage->begin());
    (*rtcStorage)[4] = MBC3_RTC_VERSION;
    std::copy(mbc3RtcRegisters.begin(), mbc3RtcRegisters.end(),
              rtcStorage->begin() + RTC_LIVE_REGISTERS_OFFSET);
    std::copy(mbc3RtcLatchedRegisters.begin(),
              mbc3RtcLatchedRegisters.end(),
              rtcStorage->begin() + RTC_LATCHED_REGISTERS_OFFSET);
    (*rtcStorage)[RTC_LAST_LATCH_VALUE_OFFSET] = mbc3RtcLastLatchValue;
    const uint64_t storedTime =
        mbc3RtcLastUnixTime < 0
            ? 0
            : static_cast<uint64_t>(mbc3RtcLastUnixTime);
    for (size_t i = 0; i < sizeof(storedTime); ++i)
        (*rtcStorage)[RTC_LAST_UNIX_TIME_OFFSET + i] =
            static_cast<uint8_t>(storedTime >> (i * 8));
}

void Cartridge::latchRtc(uint8_t value) {
    ensureRtcLoaded();
    syncRtcToNow();
    if (mbc3RtcLastLatchValue == 0 && value == 1)
        mbc3RtcLatchedRegisters = mbc3RtcRegisters;
    mbc3RtcLastLatchValue = value == 0 || value == 1 ? value : 0xFF;
    saveRtcState();
}

uint8_t Cartridge::readRtcRegister(uint8_t registerIndex) {
    syncRtcToNow();
    return mbc3RtcLatchedRegisters[registerIndex];
}

void Cartridge::writeRtcRegister(uint8_t registerIndex, uint8_t value) {
    syncRtcToNow();
    switch (registerIndex) {
    case 0:
        mbc3RtcRegisters[0] = value & 0x3F;
        break;
    case 1:
        mbc3RtcRegisters[1] = value & 0x3F;
        break;
    case 2:
        mbc3RtcRegisters[2] = value & 0x1F;
        break;
    case 3:
        mbc3RtcRegisters[3] = value;
        break;
    case 4:
        mbc3RtcRegisters[4] = value & 0xC1;
        break;
    default:
        return;
    }
    saveRtcState();
}

uint8_t Cartridge::readRom(uint16_t address) const {
    if (mapper == Mapper::RomOnly)
        return address < rom.size() ? rom[address] : 0xFF;

    const size_t bank = getSelectedRomBank(address);
    const size_t bankOffset = address < 0x4000 ? address : address - 0x4000;
    const size_t offset = bank * ROM_BANK_SIZE + bankOffset;
    return offset < rom.size() ? rom[offset] : 0xFF;
}

uint8_t Cartridge::readExternalRam(uint16_t address) {
    if (mapper == Mapper::Mbc3 && mbc3RamBank >= 0x08 &&
        mbc3RamBank <= 0x0C) {
        if (!ramEnabled || !mbc3HasRtc)
            return 0xFF;
        return readRtcRegister(mbc3RamBank - 0x08);
    }
    if (mapper == Mapper::RomOnly || !ramEnabled || !externalRam ||
        externalRam->empty())
        return 0xFF;
    if (mapper == Mapper::Mbc3 && mbc3RamBank > (mbc30 ? 0x07 : 0x03))
        return 0xFF;

    if (mapper == Mapper::Mbc2) {
        const size_t offset = (address - 0xA000) & 0x01FF;
        return 0xF0 | ((*externalRam)[offset] & 0x0F);
    }

    const size_t offset = getSelectedRamBank() * RAM_BANK_SIZE +
                          (address - 0xA000);
    return (*externalRam)[offset % externalRam->size()];
}

void Cartridge::writeExternalRam(uint16_t address, uint8_t value) {
    if (mapper == Mapper::Mbc3 && mbc3RamBank >= 0x08 &&
        mbc3RamBank <= 0x0C) {
        if (ramEnabled && mbc3HasRtc)
            writeRtcRegister(mbc3RamBank - 0x08, value);
        return;
    }
    if (!ramEnabled || !externalRam || externalRam->empty())
        return;
    if (mapper == Mapper::Mbc3 && mbc3RamBank > (mbc30 ? 0x07 : 0x03))
        return;

    if (mapper == Mapper::Mbc2) {
        const size_t offset = (address - 0xA000) & 0x01FF;
        (*externalRam)[offset] = value & 0x0F;
        return;
    }

    const size_t offset = getSelectedRamBank() * RAM_BANK_SIZE +
                          (address - 0xA000);
    (*externalRam)[offset % externalRam->size()] = value;
}

size_t Cartridge::getRomBankCount() const {
    return rom.size() / ROM_BANK_SIZE;
}

size_t Cartridge::getSelectedRomBank(uint16_t address) const {
    if (mapper == Mapper::RomOnly)
        return 0;

    if (mapper == Mapper::Mbc2)
        return address < 0x4000 ? 0 : mbc2RomBank % getRomBankCount();

    if (mapper == Mapper::Mbc3)
        return address < 0x4000 ? 0 : mbc3RomBank % getRomBankCount();

    if (mapper == Mapper::Mbc5)
        return address < 0x4000 ? 0 : mbc5RomBank % getRomBankCount();

    size_t bank = 0;
    if (address < 0x4000) {
        if (ramBankingMode)
            bank = static_cast<size_t>(bankHigh) << 5;
    } else {
        bank = (static_cast<size_t>(bankHigh) << 5) | romBankLow;
    }
    return bank % getRomBankCount();
}

size_t Cartridge::getSelectedRamBank() const {
    if (mapper == Mapper::Mbc3)
        return mbc3RamBank;
    if (mapper == Mapper::Mbc5)
        return mbc5RamBank;
    if (!ramBankingMode || !externalRam ||
        externalRam->size() <= RAM_BANK_SIZE)
        return 0;
    const size_t ramBankCount = externalRam->size() / RAM_BANK_SIZE;
    return bankHigh % ramBankCount;
}

}
