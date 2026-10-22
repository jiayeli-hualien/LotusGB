#include "include/cartridge.hpp"

#include <stdexcept>
#include <utility>

namespace LOTUSGB {
namespace {

constexpr size_t ROM_BANK_SIZE = 0x4000;
constexpr size_t RAM_BANK_SIZE = 0x2000;
constexpr size_t MINIMUM_BANKED_ROM_SIZE = 2 * ROM_BANK_SIZE;
constexpr size_t MAXIMUM_MBC_ROM_SIZE = 2 * 1024 * 1024;
constexpr size_t CARTRIDGE_TYPE_OFFSET = 0x0147;
constexpr size_t RAM_SIZE_OFFSET = 0x0149;

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
    default:
        throw std::invalid_argument("unsupported MBC3 RAM size");
    }
}

}

Cartridge::Cartridge(std::vector<uint8_t> rom,
                     std::shared_ptr<std::vector<uint8_t>> saveRam)
    : rom(std::move(rom)) {
    if (this->rom.empty())
        throw std::invalid_argument("cartridge ROM cannot be empty");

    const uint8_t type = this->rom.size() > CARTRIDGE_TYPE_OFFSET
                             ? this->rom[CARTRIDGE_TYPE_OFFSET]
                             : 0x00;
    switch (type) {
    case 0x00:
        if (this->rom.size() > 0x8000)
            throw std::invalid_argument("ROM-only cartridge exceeds 32 KiB");
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
    case 0x11:
    case 0x12:
    case 0x13:
        if (this->rom.size() < MINIMUM_BANKED_ROM_SIZE ||
            this->rom.size() > MAXIMUM_MBC_ROM_SIZE ||
            this->rom.size() % ROM_BANK_SIZE != 0)
            throw std::invalid_argument("MBC3 cartridge must contain 32 KiB to 2 MiB of banked ROM");
        mapper = Mapper::Mbc3;
        battery = type == 0x13;
        if (type == 0x12 || type == 0x13) {
            const uint8_t sizeCode = this->rom.size() > RAM_SIZE_OFFSET
                                         ? this->rom[RAM_SIZE_OFFSET]
                                         : 0x00;
            const size_t ramSize = getMbc3RamSize(sizeCode);
            if (battery && saveRam) {
                if (saveRam->size() != ramSize)
                    throw std::invalid_argument("MBC3 save RAM size does not match the cartridge header");
                externalRam = std::move(saveRam);
            } else {
                externalRam = std::make_shared<std::vector<uint8_t>>(
                    ramSize, 0xFF);
            }
        }
        return;
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

    if (address <= 0x1FFF) {
        ramEnabled = externalRam && !externalRam->empty() &&
                     (value & 0x0F) == 0x0A;
    } else if (address <= 0x3FFF) {
        if (mapper == Mapper::Mbc1) {
            romBankLow = value & 0x1F;
            if (romBankLow == 0)
                romBankLow = 1;
        } else {
            mbc3RomBank = value & 0x7F;
            if (mbc3RomBank == 0)
                mbc3RomBank = 1;
        }
    } else if (address <= 0x5FFF) {
        if (mapper == Mapper::Mbc1)
            bankHigh = value & 0x03;
        else
            mbc3RamBank = value;
    } else if (address <= 0x7FFF) {
        if (mapper == Mapper::Mbc1)
            ramBankingMode = (value & 0x01) != 0;
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

uint8_t Cartridge::readRom(uint16_t address) const {
    if (mapper == Mapper::RomOnly)
        return address < rom.size() ? rom[address] : 0xFF;

    const size_t bank = getSelectedRomBank(address);
    const size_t bankOffset = address < 0x4000 ? address : address - 0x4000;
    const size_t offset = bank * ROM_BANK_SIZE + bankOffset;
    return offset < rom.size() ? rom[offset] : 0xFF;
}

uint8_t Cartridge::readExternalRam(uint16_t address) const {
    if (mapper == Mapper::RomOnly || !ramEnabled || !externalRam ||
        externalRam->empty())
        return 0xFF;
    if (mapper == Mapper::Mbc3 && mbc3RamBank > 0x03)
        return 0xFF;

    const size_t offset = getSelectedRamBank() * RAM_BANK_SIZE +
                          (address - 0xA000);
    return (*externalRam)[offset % externalRam->size()];
}

void Cartridge::writeExternalRam(uint16_t address, uint8_t value) {
    if (!ramEnabled || !externalRam || externalRam->empty())
        return;
    if (mapper == Mapper::Mbc3 && mbc3RamBank > 0x03)
        return;

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

    if (mapper == Mapper::Mbc3)
        return address < 0x4000 ? 0 : mbc3RomBank % getRomBankCount();

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
    if (!ramBankingMode || !externalRam ||
        externalRam->size() <= RAM_BANK_SIZE)
        return 0;
    const size_t ramBankCount = externalRam->size() / RAM_BANK_SIZE;
    return bankHigh % ramBankCount;
}

}
