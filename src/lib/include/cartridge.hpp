#ifndef LOTUSGB_CARTRIDGE_HPP
#define LOTUSGB_CARTRIDGE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "address_bus.hpp"

namespace LOTUSGB {

// Cartridge support: ROM-only, MBC1 ($01-$03), MBC2 ($05-$06), MBC3
// ($0F-$13), and MBC5 ($19-$1E). MBC5 rumble motor output is not exposed.
class Cartridge final : public IAddressRegion {
public:
    using RtcNowFunction = std::function<int64_t()>;

    explicit Cartridge(
        std::vector<uint8_t> rom,
        std::shared_ptr<std::vector<uint8_t>> saveRam = nullptr,
        std::shared_ptr<std::vector<uint8_t>> saveRtc = nullptr,
        RtcNowFunction rtcNow = {});

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;

    bool hasBattery() const;
    uint8_t *getExternalRamData();
    const uint8_t *getExternalRamData() const;
    size_t getExternalRamSize() const;
    std::shared_ptr<std::vector<uint8_t>> getBatteryRamStorage() const;
    std::shared_ptr<std::vector<uint8_t>> getBatteryRtcStorage() const;

private:
    enum class Mapper {
        RomOnly,
        Mbc1,
        Mbc2,
        Mbc3,
        Mbc5,
    };

    uint8_t readRom(uint16_t address) const;
    uint8_t readExternalRam(uint16_t address);
    void writeExternalRam(uint16_t address, uint8_t value);
    void initializeRtcStorage();
    void ensureRtcLoaded();
    void syncRtcToNow();
    void saveRtcState();
    void latchRtc(uint8_t value);
    uint8_t readRtcRegister(uint8_t registerIndex);
    void writeRtcRegister(uint8_t registerIndex, uint8_t value);
    size_t getRomBankCount() const;
    size_t getSelectedRomBank(uint16_t address) const;
    size_t getSelectedRamBank() const;

    std::vector<uint8_t> rom;
    std::shared_ptr<std::vector<uint8_t>> externalRam;
    std::shared_ptr<std::vector<uint8_t>> rtcStorage;
    RtcNowFunction rtcNow;
    Mapper mapper = Mapper::RomOnly;
    bool battery = false;
    bool ramEnabled = false;
    uint8_t romBankLow = 1;
    uint8_t bankHigh = 0;
    uint8_t mbc3RomBank = 1;
    uint8_t mbc3RamBank = 0;
    std::array<uint8_t, 5> mbc3RtcRegisters{};
    std::array<uint8_t, 5> mbc3RtcLatchedRegisters{};
    int64_t mbc3RtcLastUnixTime = 0;
    uint8_t mbc3RtcLastLatchValue = 0xFF;
    bool mbc3HasRtc = false;
    bool mbc3RtcLoaded = false;
    uint8_t mbc2RomBank = 1;
    uint16_t mbc5RomBank = 1;
    uint8_t mbc5RamBank = 0;
    bool mbc5HasRumble = false;
    bool ramBankingMode = false;
};

}

#endif // LOTUSGB_CARTRIDGE_HPP
