#ifndef LOTUSGBA_GBA_CARTRIDGE_RTC_HPP
#define LOTUSGBA_GBA_CARTRIDGE_RTC_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace LOTUSGBA {

class GbaCartridgeRtc final {
public:
    static constexpr size_t STORAGE_SIZE = 24;

    explicit GbaCartridgeRtc(
        bool present,
        std::shared_ptr<std::vector<uint8_t>> storage = nullptr);

    bool present() const { return present_; }
    size_t storageSize() const;
    uint8_t *storageData();
    std::shared_ptr<std::vector<uint8_t>> storage() const { return storage_; }

    bool handlesAddress(uint32_t address) const;
    uint8_t readByte(uint32_t address, uint8_t romValue) const;
    void writeRegister(uint32_t address, uint16_t value);
    void resetPort();

private:
    static constexpr uint32_t GPIO_DATA_OFFSET = 0xC4;
    static constexpr uint32_t GPIO_DIRECTION_OFFSET = 0xC6;
    static constexpr uint32_t GPIO_CONTROL_OFFSET = 0xC8;
    static constexpr uint8_t PIN_SCK = 1u << 0;
    static constexpr uint8_t PIN_SIO = 1u << 1;
    static constexpr uint8_t PIN_CS = 1u << 2;

    void ensureInitialized() const;
    void beginTransaction();
    void endTransaction();
    void clockRisingEdge();
    void decodeCommand();
    void prepareReadPayload();
    void acceptWriteByte(uint8_t value);
    void applyWritePayload();
    void resetRtc();
    int64_t currentRtcTime() const;
    void persistRtcTime() const;
    uint8_t readGpioData() const;
    uint8_t readGpioRegister(uint32_t offset, uint8_t romValue) const;

    bool present_ = false;
    std::shared_ptr<std::vector<uint8_t>> storage_;
    mutable bool initialized_ = false;
    mutable uint8_t status_ = 0x40;
    mutable uint8_t alarmHour_ = 0;
    mutable uint8_t alarmMinute_ = 0;
    mutable int64_t rtcTimeBase_ = 0;
    mutable int64_t hostTimeBase_ = 0;

    uint8_t gpioData_ = 0;
    uint8_t gpioDirection_ = 0;
    uint8_t gpioControl_ = 0;
    bool rtcSioOutput_ = false;
    bool transactionActive_ = false;
    uint8_t commandShift_ = 0;
    uint8_t commandBitCount_ = 0;
    uint8_t command_ = 0;
    bool commandComplete_ = false;
    bool commandIsRead_ = false;
    uint8_t dataBitCount_ = 0;
    uint8_t dataByteIndex_ = 0;
    uint8_t dataShift_ = 0;
    uint8_t writeLength_ = 0;
    std::array<uint8_t, 7> readPayload_{};
    std::array<uint8_t, 7> writePayload_{};
    uint8_t readLength_ = 0;
};

} // namespace LOTUSGBA

#endif // LOTUSGBA_GBA_CARTRIDGE_RTC_HPP
