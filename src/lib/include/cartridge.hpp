#ifndef LOTUSGB_CARTRIDGE_HPP
#define LOTUSGB_CARTRIDGE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "address_bus.hpp"

namespace LOTUSGB {

// Initial cartridge support: ROM-only and the MBC1 variants (types $01-$03).
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
    uint8_t readRom(uint16_t address) const;
    uint8_t readExternalRam(uint16_t address) const;
    void writeExternalRam(uint16_t address, uint8_t value);
    size_t getRomBankCount() const;
    size_t getSelectedRomBank(uint16_t address) const;
    size_t getSelectedRamBank() const;

    std::vector<uint8_t> rom;
    std::shared_ptr<std::vector<uint8_t>> externalRam;
    bool mbc1 = false;
    bool battery = false;
    bool ramEnabled = false;
    uint8_t romBankLow = 1;
    uint8_t bankHigh = 0;
    bool ramBankingMode = false;
};

}

#endif // LOTUSGB_CARTRIDGE_HPP
