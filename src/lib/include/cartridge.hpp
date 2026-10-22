#ifndef LOTUSGB_CARTRIDGE_HPP
#define LOTUSGB_CARTRIDGE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "address_bus.hpp"

namespace LOTUSGB {

// Cartridge support: ROM-only, MBC1 ($01-$03), MBC2 ($05-$06), MBC3
// ($11-$13), and MBC5 ($19-$1E). MBC3 timer variants are not supported yet;
// MBC5 rumble motor output is not exposed.
class Cartridge final : public IAddressRegion {
public:
    explicit Cartridge(
        std::vector<uint8_t> rom,
        std::shared_ptr<std::vector<uint8_t>> saveRam = nullptr);

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;

    bool hasBattery() const;
    uint8_t *getExternalRamData();
    const uint8_t *getExternalRamData() const;
    size_t getExternalRamSize() const;
    std::shared_ptr<std::vector<uint8_t>> getBatteryRamStorage() const;

private:
    enum class Mapper {
        RomOnly,
        Mbc1,
        Mbc2,
        Mbc3,
        Mbc5,
    };

    uint8_t readRom(uint16_t address) const;
    uint8_t readExternalRam(uint16_t address) const;
    void writeExternalRam(uint16_t address, uint8_t value);
    size_t getRomBankCount() const;
    size_t getSelectedRomBank(uint16_t address) const;
    size_t getSelectedRamBank() const;

    std::vector<uint8_t> rom;
    std::shared_ptr<std::vector<uint8_t>> externalRam;
    Mapper mapper = Mapper::RomOnly;
    bool battery = false;
    bool ramEnabled = false;
    uint8_t romBankLow = 1;
    uint8_t bankHigh = 0;
    uint8_t mbc3RomBank = 1;
    uint8_t mbc3RamBank = 0;
    uint8_t mbc2RomBank = 1;
    uint16_t mbc5RomBank = 1;
    uint8_t mbc5RamBank = 0;
    bool mbc5HasRumble = false;
    bool ramBankingMode = false;
};

}

#endif // LOTUSGB_CARTRIDGE_HPP
