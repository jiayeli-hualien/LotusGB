#ifndef LOTUSGB_CARTRIDGE_HPP
#define LOTUSGB_CARTRIDGE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "address_bus.hpp"
#include "system_clock.hpp"

namespace LOTUSGB {

namespace Test {
struct CartridgeTestAccess;
}

// Cartridge support: ROM-only, MBC1 ($01-$03), MBC2 ($05-$06), MBC3
// / MBC30 ($0F-$13), MBC5 ($19-$1E), and MBC7 ($22). MBC5/MBC7 rumble is
// exposed as a host callback.
class Cartridge final : public IAddressRegion, public IClockedComponent {
    friend class StateCodec;
public:
    using RtcNowFunction = std::function<int64_t()>;

    explicit Cartridge(
        std::vector<uint8_t> rom,
        std::shared_ptr<std::vector<uint8_t>> saveRam = nullptr,
        std::shared_ptr<std::vector<uint8_t>> saveRtc = nullptr,
        RtcNowFunction rtcNow = {});

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void advanceTCycles(uint64_t tCycles) override;

    bool hasBattery() const;
    uint8_t *getExternalRamData();
    const uint8_t *getExternalRamData() const;
    size_t getExternalRamSize() const;
    void setMbc7AccelerometerData(uint16_t x, uint16_t y);
    bool isRumbleActive() const;
    static constexpr uint32_t MBC7_WORD_PROGRAM_TCYCLES = 25166; // 6 ms at 4.194304 MHz.
    static constexpr uint32_t MBC7_WRITE_ALL_TCYCLES = 62915; // 15 ms at 4.194304 MHz.
    std::shared_ptr<std::vector<uint8_t>> getBatteryRamStorage() const;
    std::shared_ptr<std::vector<uint8_t>> getBatteryRtcStorage() const;

private:
    friend class GameBoySystem;
    friend struct Test::CartridgeTestAccess;

    Cartridge(std::vector<uint8_t> rom,
              std::shared_ptr<std::vector<uint8_t>> saveRam,
              std::shared_ptr<std::vector<uint8_t>> saveRtc,
              RtcNowFunction rtcNow,
              const std::array<uint8_t, 32> *knownRomHash);

    enum class Mapper {
        RomOnly,
        Mbc1,
        Mbc2,
        Mbc3,
        Mbc5,
        Mbc7,
    };
    enum class Mbc7EepromPhase : uint8_t {
        Command,
        ReadData,
        WriteData,
        WriteAllData,
    };
    enum class Mbc7EepromPendingOperation : uint8_t {
        None,
        WriteWord,
        EraseWord,
        EraseAll,
        WriteAll,
    };

    uint8_t readRom(uint16_t address) const;
    uint8_t readExternalRam(uint16_t address);
    void writeExternalRam(uint16_t address, uint8_t value);
    uint8_t readMbc7Register(uint16_t address) const;
    void writeMbc7Register(uint16_t address, uint8_t value);
    void clockMbc7Eeprom(uint8_t value);
    void decodeMbc7EepromCommand();
    void beginMbc7EepromProgramming();
    void finishMbc7EepromProgramming();
    void completeMbc7EepromWrite(uint16_t value, bool allWords);
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
    void setRumbleCallback(std::function<void(bool)> callback);
    void notifyRumbleState();

    std::vector<uint8_t> rom;
    // ROM content is immutable; save states reuse this identity per snapshot.
    std::array<uint8_t, 32> romHash{};
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
    bool mbc1Multicart = false; // Immutable wiring detected from subgame headers.
    bool mbc30 = false; // Immutable variant derived from ROM/RAM geometry.
    bool mbc3HasRtc = false;
    bool mbc3RtcLoaded = false;
    uint8_t mbc2RomBank = 1;
    uint16_t mbc5RomBank = 1;
    uint8_t mbc5RamBank = 0;
    bool mbc5HasRumble = false;
    std::function<void(bool)> rumbleCallback;
    bool ramBankingMode = false;
    bool mbc7RamEnable1 = false;
    uint8_t mbc7ControlRegister = 0;
    uint8_t mbc7RomBank = 1;
    uint16_t mbc7AccelerometerX = 0x81D0;
    uint16_t mbc7AccelerometerY = 0x81D0;
    uint16_t mbc7LatchedAccelerometerX = 0x8000;
    uint16_t mbc7LatchedAccelerometerY = 0x8000;
    bool mbc7AccelerometerLatchArmed = false;
    bool mbc7AccelerometerLatched = false;
    bool mbc7EepromWriteEnabled = false;
    bool mbc7EepromChipSelect = false;
    bool mbc7EepromClock = false;
    bool mbc7EepromDataIn = false;
    bool mbc7EepromDataOut = true;
    Mbc7EepromPhase mbc7EepromPhase = Mbc7EepromPhase::Command;
    uint16_t mbc7EepromShiftRegister = 0;
    uint16_t mbc7EepromReadWord = 0xFFFF;
    uint8_t mbc7EepromBitCount = 0;
    uint8_t mbc7EepromAddress = 0;
    uint8_t mbc7EepromReadBitsRemaining = 0;
    Mbc7EepromPendingOperation mbc7EepromPendingOperation = Mbc7EepromPendingOperation::None;
    uint16_t mbc7EepromPendingWord = 0xFFFF;
    uint32_t mbc7EepromBusyTCycles = 0;
    uint8_t mbc7EepromChipSelectLowTCycles = 2;
    bool mbc7EepromChipSelectQualified = false;
};

}

#endif // LOTUSGB_CARTRIDGE_HPP
